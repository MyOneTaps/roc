/*
 * Copyright (c) 2015 Roc authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_audio/packetizer.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"
#include "roc_core/random.h"

namespace roc {
namespace audio {

Packetizer::Packetizer(packet::IWriter& writer,
                       packet::IComposer& composer,
                       IEncoder& encoder,
                       packet::PacketPool& packet_pool,
                       core::BufferPool<uint8_t>& buffer_pool,
                       packet::channel_mask_t channels,
                       core::nanoseconds_t packet_length,
                       size_t sample_rate,
                       unsigned int payload_type)
    : writer_(writer)
    , composer_(composer)
    , encoder_(encoder)
    , packet_pool_(packet_pool)
    , buffer_pool_(buffer_pool)
    , channels_(channels)
    , num_channels_(packet::num_channels(channels))
    , samples_per_packet_(
          (packet::timestamp_t)packet::timestamp_from_ns(packet_length, sample_rate))
    , payload_type_(payload_type)
    , packet_pos_(0)
    , source_((packet::source_t)core::random(packet::source_t(-1)))
    , seqnum_((packet::seqnum_t)core::random(packet::seqnum_t(-1)))
    , timestamp_((packet::timestamp_t)core::random(packet::timestamp_t(-1))) {
}

void Packetizer::write(Frame& frame) {
    if (frame.size() % num_channels_ != 0) {
        roc_panic("packetizer: unexpected frame size");
    }

    const sample_t* buffer_ptr = frame.data();
    size_t buffer_samples = frame.size() / num_channels_;

    while (buffer_samples != 0) {
        if (!packet_) {
            if (!(packet_ = start_packet_())) {
                return;
            }
        }

        size_t ns = encoder_.write_samples(*packet_, packet_pos_, buffer_ptr,
                                           buffer_samples, channels_);

        packet_pos_ += ns;
        buffer_samples -= ns;
        buffer_ptr += ns * num_channels_;

        if (packet_pos_ == samples_per_packet_) {
            flush();
        }
    }
}

void Packetizer::flush() {
    if (packet_pos_ == 0) {
        return;
    }

    if (finish_packet_()) {
        writer_.write(packet_);
    }

    seqnum_++;
    timestamp_ += (packet::timestamp_t)packet_pos_;

    packet_pos_ = 0;
    packet_ = NULL;
}

packet::PacketPtr Packetizer::start_packet_() {
    packet::PacketPtr packet = new (packet_pool_) packet::Packet(packet_pool_);
    if (!packet) {
        roc_log(LogError, "packetizer: can't allocate packet");
        return NULL;
    }

    packet->add_flags(packet::Packet::FlagAudio);

    core::Slice<uint8_t> data = new (buffer_pool_) core::Buffer<uint8_t>(buffer_pool_);
    if (!data) {
        roc_log(LogError, "packetizer: can't allocate buffer");
        return NULL;
    }

    if (!composer_.prepare(*packet, data, encoder_.payload_size(samples_per_packet_))) {
        roc_log(LogError, "packetizer: can't prepare packet");
        return NULL;
    }

    packet->set_data(data);

    packet::RTP* rtp = packet->rtp();
    if (!rtp) {
        roc_log(LogError, "packetizer: unexpected non-rtp packet");
        return NULL;
    }

    rtp->source = source_;
    rtp->seqnum = seqnum_;
    rtp->timestamp = timestamp_;
    rtp->payload_type = payload_type_;

    return packet;
}

bool Packetizer::finish_packet_() {
    if (packet_pos_ == samples_per_packet_) {
        return true;
    }

    if (!composer_.truncate(*packet_, encoder_.payload_size(packet_pos_))) {
        roc_log(LogError, "packetizer: can't truncate packet");
        return false;
    }

    return true;
}

} // namespace audio
} // namespace roc
