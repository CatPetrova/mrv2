// SPDX-License-Identifier: BSD-3-Clause
// mrv2
// Copyright Contributors to the mrv2 Project. All rights reserved.

#include "mrvCore/mrvFastMovieCopy.h"

#include <tlIO/FFmpeg.h>

#include <tlCore/StringFormat.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

extern "C"
{
#include <libavformat/avformat.h>
}

namespace
{
    struct FormatInput
    {
        ~FormatInput()
        {
            if (ctx)
                avformat_close_input(&ctx);
        }

        AVFormatContext* ctx = nullptr;
    };

    struct FormatOutput
    {
        ~FormatOutput()
        {
            if (ctx)
            {
                if (ctx->pb)
                    avio_closep(&ctx->pb);
                avformat_free_context(ctx);
            }
        }

        AVFormatContext* ctx = nullptr;
    };

    struct Packet
    {
        Packet() : p(av_packet_alloc()) {}
        ~Packet() { av_packet_free(&p); }

        AVPacket* p = nullptr;
    };

    int64_t to_us(const tl::otime::RationalTime& time)
    {
        return static_cast<int64_t>(
            std::llround(time.to_seconds() * static_cast<double>(AV_TIME_BASE)));
    }

    int64_t packet_time_us(const AVPacket* packet, const AVStream* stream)
    {
        int64_t timestamp = packet->pts;
        if (timestamp == AV_NOPTS_VALUE)
            timestamp = packet->dts;
        if (timestamp == AV_NOPTS_VALUE)
            return AV_NOPTS_VALUE;
        return av_rescale_q(timestamp, stream->time_base, AV_TIME_BASE_Q);
    }

    bool should_copy_stream(const AVStream* stream)
    {
        const auto type = stream->codecpar->codec_type;
        return type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO;
    }
}

namespace mrv
{
    void fast_movie_copy(
        const std::string& inputFile, const std::string& outputFile,
        const tl::otime::RationalTime& startTime,
        const tl::otime::RationalTime& endTimeExclusive)
    {
        using namespace tl;

        FormatInput input;
        int r = avformat_open_input(&input.ctx, inputFile.c_str(), nullptr, nullptr);
        if (r < 0)
        {
            throw std::runtime_error(
                string::Format("{0}: Cannot open input - {1}.")
                    .arg(inputFile)
                    .arg(ffmpeg::getErrorLabel(r)));
        }

        r = avformat_find_stream_info(input.ctx, nullptr);
        if (r < 0)
        {
            throw std::runtime_error(
                string::Format("{0}: Cannot read stream info - {1}.")
                    .arg(inputFile)
                    .arg(ffmpeg::getErrorLabel(r)));
        }

        FormatOutput output;
        r = avformat_alloc_output_context2(
            &output.ctx, nullptr, nullptr, outputFile.c_str());
        if (r < 0 || !output.ctx)
        {
            throw std::runtime_error(
                string::Format("{0}: Cannot create output context - {1}.")
                    .arg(outputFile)
                    .arg(ffmpeg::getErrorLabel(r)));
        }

        std::vector<int> streamMap(input.ctx->nb_streams, -1);
        int outputStreamCount = 0;
        for (unsigned int i = 0; i < input.ctx->nb_streams; ++i)
        {
            AVStream* inStream = input.ctx->streams[i];
            if (!should_copy_stream(inStream))
                continue;

            AVStream* outStream = avformat_new_stream(output.ctx, nullptr);
            if (!outStream)
            {
                throw std::runtime_error(
                    string::Format("{0}: Cannot allocate output stream.")
                        .arg(outputFile));
            }

            streamMap[i] = outputStreamCount++;
            r = avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
            if (r < 0)
            {
                throw std::runtime_error(
                    string::Format("{0}: Cannot copy codec parameters - {1}.")
                        .arg(outputFile)
                        .arg(ffmpeg::getErrorLabel(r)));
            }
            outStream->codecpar->codec_tag = 0;
            outStream->time_base = inStream->time_base;
        }

        if (outputStreamCount == 0)
        {
            throw std::runtime_error(
                string::Format("{0}: No video or audio streams to copy.")
                    .arg(inputFile));
        }

        const int64_t startUs = std::max<int64_t>(0, to_us(startTime));
        const int64_t endUs = std::max<int64_t>(startUs, to_us(endTimeExclusive));

        r = av_seek_frame(input.ctx, -1, startUs, AVSEEK_FLAG_BACKWARD);
        if (r < 0)
        {
            throw std::runtime_error(
                string::Format("{0}: Cannot seek for fast copy - {1}.")
                    .arg(inputFile)
                    .arg(ffmpeg::getErrorLabel(r)));
        }

        if (!(output.ctx->oformat->flags & AVFMT_NOFILE))
        {
            r = avio_open(&output.ctx->pb, outputFile.c_str(), AVIO_FLAG_WRITE);
            if (r < 0)
            {
                throw std::runtime_error(
                    string::Format("{0}: Cannot open output - {1}.")
                        .arg(outputFile)
                        .arg(ffmpeg::getErrorLabel(r)));
            }
        }

        r = avformat_write_header(output.ctx, nullptr);
        if (r < 0)
        {
            throw std::runtime_error(
                string::Format("{0}: Cannot write header - {1}.")
                    .arg(outputFile)
                    .arg(ffmpeg::getErrorLabel(r)));
        }

        Packet packet;
        if (!packet.p)
        {
            throw std::runtime_error(
                string::Format("{0}: Cannot allocate packet.").arg(outputFile));
        }

        std::vector<int64_t> firstPts(input.ctx->nb_streams, AV_NOPTS_VALUE);
        std::vector<int64_t> firstDts(input.ctx->nb_streams, AV_NOPTS_VALUE);

        while ((r = av_read_frame(input.ctx, packet.p)) >= 0)
        {
            const unsigned int inStreamIndex = packet.p->stream_index;
            if (inStreamIndex >= streamMap.size() || streamMap[inStreamIndex] < 0)
            {
                av_packet_unref(packet.p);
                continue;
            }

            AVStream* inStream = input.ctx->streams[inStreamIndex];
            const int64_t packetUs = packet_time_us(packet.p, inStream);
            if (packetUs != AV_NOPTS_VALUE && packetUs >= endUs)
            {
                av_packet_unref(packet.p);
                break;
            }

            AVStream* outStream =
                output.ctx->streams[streamMap[inStreamIndex]];

            if (firstPts[inStreamIndex] == AV_NOPTS_VALUE &&
                packet.p->pts != AV_NOPTS_VALUE)
            {
                firstPts[inStreamIndex] = packet.p->pts;
            }
            if (firstDts[inStreamIndex] == AV_NOPTS_VALUE &&
                packet.p->dts != AV_NOPTS_VALUE)
            {
                firstDts[inStreamIndex] = packet.p->dts;
            }

            if (packet.p->pts != AV_NOPTS_VALUE &&
                firstPts[inStreamIndex] != AV_NOPTS_VALUE)
            {
                packet.p->pts -= firstPts[inStreamIndex];
            }
            if (packet.p->dts != AV_NOPTS_VALUE &&
                firstDts[inStreamIndex] != AV_NOPTS_VALUE)
            {
                packet.p->dts -= firstDts[inStreamIndex];
            }

            av_packet_rescale_ts(
                packet.p, inStream->time_base, outStream->time_base);
            packet.p->stream_index = streamMap[inStreamIndex];
            packet.p->pos = -1;

            r = av_interleaved_write_frame(output.ctx, packet.p);
            av_packet_unref(packet.p);
            if (r < 0)
            {
                throw std::runtime_error(
                    string::Format("{0}: Cannot write packet - {1}.")
                        .arg(outputFile)
                        .arg(ffmpeg::getErrorLabel(r)));
            }
        }

        if (r < 0 && r != AVERROR_EOF)
        {
            throw std::runtime_error(
                string::Format("{0}: Cannot read packet - {1}.")
                    .arg(inputFile)
                    .arg(ffmpeg::getErrorLabel(r)));
        }

        r = av_write_trailer(output.ctx);
        if (r < 0)
        {
            throw std::runtime_error(
                string::Format("{0}: Cannot write trailer - {1}.")
                    .arg(outputFile)
                    .arg(ffmpeg::getErrorLabel(r)));
        }
    }
}
