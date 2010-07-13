//
// Copyright 2010 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#ifndef INCLUDED_LIBUHD_TRANSPORT_VRT_PACKET_HANDLER_HPP
#define INCLUDED_LIBUHD_TRANSPORT_VRT_PACKET_HANDLER_HPP

#include <uhd/config.hpp>
#include <uhd/device.hpp>
#include <uhd/utils/assert.hpp>
#include <uhd/utils/byteswap.hpp>
#include <uhd/types/io_type.hpp>
#include <uhd/types/otw_type.hpp>
#include <uhd/types/metadata.hpp>
#include <uhd/transport/vrt_if_packet.hpp>
#include <uhd/transport/convert_types.hpp>
#include <uhd/transport/zero_copy.hpp>
#include <boost/function.hpp>
#include <stdexcept>
#include <iostream>
#include <vector>

namespace vrt_packet_handler{

/***********************************************************************
 * vrt packet handler for recv
 **********************************************************************/
    typedef std::vector<uhd::transport::managed_recv_buffer::sptr> managed_recv_buffs_t;
    typedef boost::function<bool(managed_recv_buffs_t &)> get_recv_buffs_t;
    typedef boost::function<void(size_t /*which channel*/)> handle_overrun_t;

    static inline void handle_overrun_nop(size_t){}

    struct recv_state{
        //width of the receiver in channels
        size_t width;

        //state variables to handle fragments
        managed_recv_buffs_t managed_buffs;
        std::vector<const boost::uint8_t *> copy_buffs;
        size_t size_of_copy_buffs;
        size_t fragment_offset_in_samps;

        recv_state(size_t width = 1):
            width(width),
            managed_buffs(width),
            copy_buffs(width, NULL),
            size_of_copy_buffs(0),
            fragment_offset_in_samps(0)
        {
            /* NOP */
        }
    };

    /*******************************************************************
     * Unpack a received vrt header and set the copy buffer.
     *  - helper function for vrt_packet_handler::_recv1
     ******************************************************************/
    template<typename vrt_unpacker_type>
    static UHD_INLINE void _recv1_helper(
        recv_state &state,
        uhd::rx_metadata_t &metadata,
        double tick_rate,
        vrt_unpacker_type vrt_unpacker,
        const handle_overrun_t &handle_overrun,
        size_t vrt_header_offset_words32
    ){
        //vrt unpack each managed buffer
        uhd::transport::vrt::if_packet_info_t if_packet_info;
        for (size_t i = 0; i < state.width; i++){

            //extract packet words and check thats its enough to move on
            size_t num_packet_words32 = state.managed_buffs[i]->size()/sizeof(boost::uint32_t);
            if (num_packet_words32 <= vrt_header_offset_words32){
                throw std::runtime_error("recv buffer smaller than vrt packet offset");
            }

            //unpack the vrt header into the info struct
            const boost::uint32_t *vrt_hdr = state.managed_buffs[i]->cast<const boost::uint32_t *>() + vrt_header_offset_words32;
            if_packet_info.num_packet_words32 = num_packet_words32 - vrt_header_offset_words32;
            vrt_unpacker(vrt_hdr, if_packet_info);
            const boost::uint32_t *vrt_data = vrt_hdr + if_packet_info.num_header_words32;

            //handle the non-data packet case and parse its contents
            if (if_packet_info.packet_type != uhd::transport::vrt::if_packet_info_t::PACKET_TYPE_DATA){

                //extract the context word (we dont know the endianness so mirror the bytes)
                boost::uint32_t word0 = vrt_data[0] | uhd::byteswap(vrt_data[0]);
                if (word0 & uhd::rx_metadata_t::ERROR_CODE_OVERRUN) handle_overrun(i);
                metadata.error_code = uhd::rx_metadata_t::error_code_t(word0 & 0xf);

                //break to exit loop and store metadata below
                state.size_of_copy_buffs = 0; break;
            }

            //setup the buffer to point to the data
            state.copy_buffs[i] = reinterpret_cast<const boost::uint8_t *>(vrt_data);

            //store the minimum payload length into the copy buffer length
            size_t num_payload_bytes = if_packet_info.num_payload_words32*sizeof(boost::uint32_t);
            if (i == 0 or state.size_of_copy_buffs > num_payload_bytes){
                state.size_of_copy_buffs = num_payload_bytes;
            }
        }

        //store the last vrt info into the metadata
        metadata.has_time_spec = if_packet_info.has_tsi and if_packet_info.has_tsf;
        metadata.time_spec = uhd::time_spec_t(
            time_t(if_packet_info.tsi), size_t(if_packet_info.tsf), tick_rate
        );
        static const int tlr_sob_flags = (1 << 21) | (1 << 9); //enable and indicator bits
        metadata.start_of_burst = if_packet_info.has_tlr and (int(if_packet_info.tlr & tlr_sob_flags) == tlr_sob_flags);
        static const int tlr_eob_flags = (1 << 20) | (1 << 8); //enable and indicator bits
        metadata.end_of_burst   = if_packet_info.has_tlr and (int(if_packet_info.tlr & tlr_eob_flags) == tlr_eob_flags);
    }

    /*******************************************************************
     * Recv data, unpack a vrt header, and copy-convert the data.
     *  - helper function for vrt_packet_handler::recv
     ******************************************************************/
    template<typename vrt_unpacker_type>
    static UHD_INLINE size_t _recv1(
        recv_state &state,
        const std::vector<void *> &buffs,
        size_t offset_bytes,
        size_t total_samps,
        uhd::rx_metadata_t &metadata,
        const uhd::io_type_t &io_type,
        const uhd::otw_type_t &otw_type,
        double tick_rate,
        vrt_unpacker_type vrt_unpacker,
        const get_recv_buffs_t &get_recv_buffs,
        const handle_overrun_t &handle_overrun,
        size_t vrt_header_offset_words32
    ){
        metadata.error_code = uhd::rx_metadata_t::ERROR_CODE_NONE;

        //perform a receive if no rx data is waiting to be copied
        if (state.size_of_copy_buffs == 0){
            state.fragment_offset_in_samps = 0;
            if (not get_recv_buffs(state.managed_buffs)){
                metadata.error_code = uhd::rx_metadata_t::ERROR_CODE_TIMEOUT;
                return 0;
            }
            try{
                _recv1_helper(
                    state, metadata, tick_rate,
                    vrt_unpacker, handle_overrun,
                    vrt_header_offset_words32
                );
            }catch(const std::exception &e){
                state.size_of_copy_buffs = 0; //reset copy buffs size
                std::cerr << "Error (recv): " << e.what() << std::endl;
                metadata.error_code = uhd::rx_metadata_t::ERROR_CODE_BAD_PACKET;
                return 0;
            }
        }
        //defaults for the metadata when this is a fragment
        else{
            metadata.has_time_spec = false;
            metadata.start_of_burst = false;
            metadata.end_of_burst = false;
        }

        //extract the number of samples available to copy
        size_t bytes_per_item = otw_type.get_sample_size();
        size_t nsamps_available = state.size_of_copy_buffs/bytes_per_item;
        size_t nsamps_to_copy = std::min(total_samps, nsamps_available);
        size_t bytes_to_copy = nsamps_to_copy*bytes_per_item;

        for (size_t i = 0; i < state.width; i++){
            //copy-convert the samples from the recv buffer
            uhd::transport::convert_otw_type_to_io_type(
                state.copy_buffs[i], otw_type,
                reinterpret_cast<boost::uint8_t *>(buffs[i]) + offset_bytes,
                io_type, nsamps_to_copy
            );

            //update the rx copy buffer to reflect the bytes copied
            state.copy_buffs[i] += bytes_to_copy;
        }
        //update the copy buffer's availability
        state.size_of_copy_buffs -= bytes_to_copy;

        //setup the fragment flags and offset
        metadata.more_fragments = state.size_of_copy_buffs != 0;
        metadata.fragment_offset = state.fragment_offset_in_samps;
        state.fragment_offset_in_samps += nsamps_to_copy; //set for next call

        return nsamps_to_copy;
    }

    /*******************************************************************
     * Recv vrt packets and copy convert the samples into the buffer.
     ******************************************************************/
    template<typename vrt_unpacker_type>
    static UHD_INLINE size_t recv(
        recv_state &state,
        const std::vector<void *> &buffs,
        const size_t total_num_samps,
        uhd::rx_metadata_t &metadata,
        uhd::device::recv_mode_t recv_mode,
        const uhd::io_type_t &io_type,
        const uhd::otw_type_t &otw_type,
        double tick_rate,
        vrt_unpacker_type vrt_unpacker,
        const get_recv_buffs_t &get_recv_buffs,
        const handle_overrun_t &handle_overrun = &handle_overrun_nop,
        size_t vrt_header_offset_words32 = 0
    ){
        switch(recv_mode){

        ////////////////////////////////////////////////////////////////
        case uhd::device::RECV_MODE_ONE_PACKET:{
        ////////////////////////////////////////////////////////////////
            return _recv1(
                state,
                buffs, 0,
                total_num_samps,
                metadata,
                io_type, otw_type,
                tick_rate,
                vrt_unpacker,
                get_recv_buffs,
                handle_overrun,
                vrt_header_offset_words32
            );
        }

        ////////////////////////////////////////////////////////////////
        case uhd::device::RECV_MODE_FULL_BUFF:{
        ////////////////////////////////////////////////////////////////
            size_t accum_num_samps = 0;
            uhd::rx_metadata_t tmp_md;
            while(accum_num_samps < total_num_samps){
                size_t num_samps = _recv1(
                    state,
                    buffs, accum_num_samps*io_type.size,
                    total_num_samps - accum_num_samps,
                    (accum_num_samps == 0)? metadata : tmp_md, //only the first metadata gets kept
                    io_type, otw_type,
                    tick_rate,
                    vrt_unpacker,
                    get_recv_buffs,
                    handle_overrun,
                    vrt_header_offset_words32
                );
                if (num_samps == 0) break; //had a recv timeout or error, break loop
                accum_num_samps += num_samps;
            }
            return accum_num_samps;
        }

        default: throw std::runtime_error("unknown recv mode");
        }//switch(recv_mode)
    }

/***********************************************************************
 * vrt packet handler for send
 **********************************************************************/
    typedef std::vector<uhd::transport::managed_send_buffer::sptr> managed_send_buffs_t;
    typedef boost::function<bool(managed_send_buffs_t &)> get_send_buffs_t;

    struct send_state{
        //init the expected seq number
        size_t next_packet_seq;

        send_state(void) : next_packet_seq(0){
            /* NOP */
        }
    };

    /*******************************************************************
     * Pack a vrt header, copy-convert the data, and send it.
     *  - helper function for vrt_packet_handler::send
     ******************************************************************/
    template<typename vrt_packer_type>
    static UHD_INLINE void _send1(
        send_state &state,
        const std::vector<const void *> &buffs,
        size_t offset_bytes,
        size_t num_samps,
        uhd::transport::vrt::if_packet_info_t &if_packet_info,
        const uhd::io_type_t &io_type,
        const uhd::otw_type_t &otw_type,
        vrt_packer_type vrt_packer,
        const get_send_buffs_t &get_send_buffs,
        size_t vrt_header_offset_words32
    ){
        //load the rest of the if_packet_info in here
        if_packet_info.num_payload_words32 = (num_samps*otw_type.get_sample_size())/sizeof(boost::uint32_t);
        if_packet_info.packet_count = state.next_packet_seq++;

        //get send buffers for each channel
        managed_send_buffs_t send_buffs(buffs.size());
        UHD_ASSERT_THROW(get_send_buffs(send_buffs));

        for (size_t i = 0; i < buffs.size(); i++){
            //calculate pointers with offsets to io and otw memory
            const boost::uint8_t *io_mem = reinterpret_cast<const boost::uint8_t *>(buffs[i]) + offset_bytes;
            boost::uint32_t *otw_mem = send_buffs[i]->cast<boost::uint32_t *>() + vrt_header_offset_words32;

            //pack metadata into a vrt header
            vrt_packer(otw_mem, if_packet_info);

            //copy-convert the samples into the send buffer
            uhd::transport::convert_io_type_to_otw_type(
                io_mem, io_type,
                otw_mem + if_packet_info.num_header_words32, otw_type,
                num_samps
            );

            //commit the samples to the zero-copy interface
            if (send_buffs[i]->commit(if_packet_info.num_packet_words32*sizeof(boost::uint32_t)) < ssize_t(num_samps)){
                std::cerr << "commit to send buffer returned less than commit size" << std::endl;
            }
        }
    }

    /*******************************************************************
     * Send vrt packets and copy convert the samples into the buffer.
     ******************************************************************/
    template<typename vrt_packer_type>
    static UHD_INLINE size_t send(
        send_state &state,
        const std::vector<const void *> &buffs,
        const size_t total_num_samps,
        const uhd::tx_metadata_t &metadata,
        uhd::device::send_mode_t send_mode,
        const uhd::io_type_t &io_type,
        const uhd::otw_type_t &otw_type,
        double tick_rate,
        vrt_packer_type vrt_packer,
        const get_send_buffs_t &get_send_buffs,
        size_t max_samples_per_packet,
        size_t vrt_header_offset_words32 = 0
    ){
        //translate the metadata to vrt if packet info
        uhd::transport::vrt::if_packet_info_t if_packet_info;
        if_packet_info.has_sid = false;
        if_packet_info.has_cid = false;
        if_packet_info.has_tlr = false;
        if_packet_info.tsi = boost::uint32_t(metadata.time_spec.get_full_secs());
        if_packet_info.tsf = boost::uint64_t(metadata.time_spec.get_tick_count(tick_rate));

        if (total_num_samps <= max_samples_per_packet) send_mode = uhd::device::SEND_MODE_ONE_PACKET;
        switch(send_mode){

        ////////////////////////////////////////////////////////////////
        case uhd::device::SEND_MODE_ONE_PACKET:{
        ////////////////////////////////////////////////////////////////
            size_t num_samps = std::min(total_num_samps, max_samples_per_packet);

            //fill in parts of the packet info overwrote in full buff mode
            if_packet_info.has_tsi = metadata.has_time_spec;
            if_packet_info.has_tsf = metadata.has_time_spec;
            if_packet_info.sob = metadata.start_of_burst;
            if_packet_info.eob = metadata.end_of_burst;

            _send1(
                state,
                buffs, 0,
                num_samps,
                if_packet_info,
                io_type, otw_type,
                vrt_packer,
                get_send_buffs,
                vrt_header_offset_words32
            );
            return num_samps;
        }

        ////////////////////////////////////////////////////////////////
        case uhd::device::SEND_MODE_FULL_BUFF:{
        ////////////////////////////////////////////////////////////////
            //calculate constants for fragmentation
            const size_t num_fragments = (total_num_samps+max_samples_per_packet-1)/max_samples_per_packet;
            static const size_t first_fragment_index = 0;
            const size_t final_fragment_index = num_fragments-1;

            //loop through the following fragment indexes
            for (size_t n = first_fragment_index; n <= final_fragment_index; n++){

                //calculate new flags for the fragments
                if_packet_info.has_tsi = metadata.has_time_spec  and (n == first_fragment_index);
                if_packet_info.has_tsf = metadata.has_time_spec  and (n == first_fragment_index);
                if_packet_info.sob     = metadata.start_of_burst and (n == first_fragment_index);
                if_packet_info.eob     = metadata.end_of_burst   and (n == final_fragment_index);

                //send the fragment with the helper function
                _send1(
                    state,
                    buffs, n*max_samples_per_packet*io_type.size,
                    (n == final_fragment_index)?(total_num_samps%max_samples_per_packet):max_samples_per_packet,
                    if_packet_info,
                    io_type, otw_type,
                    vrt_packer,
                    get_send_buffs,
                    vrt_header_offset_words32
                );
            }
            return total_num_samps;
        }

        default: throw std::runtime_error("unknown send mode");
        }//switch(send_mode)
    }

} //namespace vrt_packet_handler

#endif /* INCLUDED_LIBUHD_TRANSPORT_VRT_PACKET_HANDLER_HPP */