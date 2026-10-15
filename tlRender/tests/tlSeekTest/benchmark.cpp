// SPDX-License-Identifier: BSD-3-Clause
// Core-only seek latency probe; see README.md for fixture requirements.

#include <tlCore/Context.h>
#include <tlIO/System.h>
#include <tlIO/Cache.h>
#include <tlTimeline/Init.h>
#include <tlTimeline/Player.h>
#include <tlTimeline/Timeline.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>

using Clock = std::chrono::steady_clock;
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    try {
        auto context = tl::system::Context::create();
        tl::timeline::init(context);
        tl::timeline::Options timelineOptions;
        if (const char* n = std::getenv("MRV_PROBE_REQUEST_COUNT")) timelineOptions.videoRequestMax = std::stoul(n);
        auto timeline = tl::timeline::Timeline::create(context, std::string(argv[1]), timelineOptions);
        tl::timeline::PlayerOptions options;
        options.playback = tl::timeline::Playback::Stop;
        options.cache.readAhead = tl::otime::RationalTime(std::stod(argv[3]), 1);
        options.cache.readBehind = tl::otime::RationalTime(std::stod(argv[4]), 1);
        auto player = tl::timeline::Player::create(timeline, context, options);
        player->setMute(true);
        auto tick = [&] {
            context->tick();
            player->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        };
        const auto start = player->getTimeRange().start_time();
        const double rate = player->getTimeRange().duration().rate();
        player->seek(start + tl::otime::RationalTime(60, rate));
        auto warm = Clock::now();
        while (Clock::now() - warm < std::chrono::milliseconds(700)) tick();
        const bool playing = std::string(argv[2]) == "forward";
        if (playing) player->setPlayback(tl::timeline::Playback::Forward);
        for (int frame : {741, 372, 783, 249, 516, 681}) {
            auto target = start + tl::otime::RationalTime(frame, rate);
            auto before = player->getCurrentTime();
            auto begin = Clock::now();
            player->seek(target);
            bool found = false;
            double output = -1;
            while (Clock::now() - begin < std::chrono::seconds(5)) {
                tick();
                const auto& video = player->getCurrentVideo();
                if (!video.empty() && !video[0].layers.empty() && video[0].layers[0].image) {
                    output = (video[0].time - start).to_frames();
                    if (output >= frame && output < frame + (playing ? 30 : 1)) {
                        found = true;
                        break;
                    }
                }
            }
            auto elapsed = std::chrono::duration<double, std::milli>(Clock::now()-begin).count();
            std::cout << "{\"mode\":\"" << argv[2] << "\",\"ahead\":" << argv[3]
                      << ",\"behind\":" << argv[4] << ",\"request_count\":" << timelineOptions.videoRequestMax << ",\"from\":" << (before-start).to_frames()
                      << ",\"target\":" << frame << ",\"output\":" << output
                      << ",\"core_latency_ms\":" << elapsed << ",\"ok\":" << (found?"true":"false")
                      << "}" << std::endl;
            // Allow only 40ms of prefetch before the next abrupt click.
            auto next = Clock::now();
            while (Clock::now()-next < std::chrono::milliseconds(40)) tick();
        }
        player->setPlayback(tl::timeline::Playback::Stop);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
