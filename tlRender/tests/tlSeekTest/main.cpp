// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026

#include <tlTimeline/Init.h>
#include <tlTimeline/Player.h>
#include <tlTimeline/Timeline.h>
#include <tlIO/System.h>
#if defined(TLRENDER_FFMPEG)
#include <tlIO/FFmpeg.h>
#endif
#include <tlCore/Context.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace tl;
using namespace std::chrono_literals;

namespace
{
    void check(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    template<typename T> T get(std::future<T>& future)
    {
        check(future.wait_for(5s) == std::future_status::ready, "request timed out");
        return future.get();
    }

    struct Pending
    {
        std::promise<io::VideoData> promise;
        std::shared_ptr<io::CancellationToken> token;
    };

    // A reader deliberately holding a future unresolved. Cancellation must
    // release the timeline's single in-flight slot without waiting for it,
    // and must reach the exact token given to the reader.
    class HeldRead : public io::IVideoRead
    {
    public:
        static std::shared_ptr<HeldRead> create(const file::Path& path)
        {
            auto out = std::shared_ptr<HeldRead>(new HeldRead);
            out->_init(path, {}, {}, nullptr);
            return out;
        }

        std::future<io::Info> getInfo() override
        {
            io::Info info;
            info.video.push_back(image::Info(16, 16, image::PixelType::RGB_U8));
            info.videoTime = otime::TimeRange(otime::RationalTime(0, 30), otime::RationalTime(900, 30));
            std::promise<io::Info> promise;
            promise.set_value(info);
            return promise.get_future();
        }

        std::future<io::VideoData> readVideo(
            const otime::RationalTime& time, const io::Options& options) override
        {
            return readVideo(time, options, nullptr);
        }

        std::future<io::VideoData> readVideo(
            const otime::RationalTime& time, const io::Options&,
            const std::shared_ptr<io::CancellationToken>& token) override
        {
            auto pending = std::make_shared<Pending>();
            pending->token = token;
            auto future = pending->promise.get_future();
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(time.to_frames());
            if (time.to_frames() == 10)
                held = pending;
            else
                pending->promise.set_value(io::VideoData());
            return future;
        }

        void cancelRequests() override {}

        std::shared_ptr<Pending> getHeld()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return held;
        }

        std::vector<int> getOrder()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return order;
        }

    private:
        std::mutex mutex;
        std::shared_ptr<Pending> held;
        std::vector<int> order;
    };

    class HeldPlugin : public io::IReadPlugin
    {
    public:
        static std::shared_ptr<HeldPlugin> create()
        {
            auto out = std::shared_ptr<HeldPlugin>(new HeldPlugin);
            out->_init("SeekTest", {{".seektest", io::FileType::Movie}}, nullptr);
            return out;
        }
        std::shared_ptr<io::IVideoRead> videoRead(const file::Path& path, const io::Options&) override
        {
            reader = HeldRead::create(path);
            return reader;
        }
        std::shared_ptr<io::IVideoRead> videoRead(
            const file::Path& path, const std::vector<file::MemoryRead>&,
            const io::Options& options) override { return videoRead(path, options); }
        std::shared_ptr<HeldRead> reader;
    };

    std::shared_ptr<Pending> waitHeld(const std::shared_ptr<HeldRead>& reader)
    {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (auto held = reader->getHeld()) return held;
            std::this_thread::sleep_for(1ms);
        }
        throw std::runtime_error("reader was not dispatched");
    }

    void scheduling(const std::shared_ptr<system::Context>& context, const std::shared_ptr<io::ReadSystem>& io)
    {
        auto plugin = HeldPlugin::create();
        io->addPlugin(plugin);
        timeline::Options options;
        options.videoRequestMax = 1;
        auto make = [&] { return timeline::Timeline::create(context, "virtual.seektest", options); };
        {
            auto timeline = make();
            auto first = timeline->getVideo(otime::RationalTime(10, 30));
            auto held = waitHeld(plugin->reader);
            auto pending = timeline->getVideo(otime::RationalTime(20, 30));
            timeline->cancelRequests({pending.id});
            check(get(pending.future).layers.empty(), "pending cancellation returned data");
            check(!held->token->isCancelled(), "cancellation affected another request");
            timeline->cancelRequests({first.id});
            check(get(first.future).layers.empty(), "in-flight cancellation returned data");
            check(held->token->isCancelled(), "reader did not receive cancellation");
            auto latest = timeline->getVideo(otime::RationalTime(80, 30));
            get(latest.future); // The old reader future is still held here.
            held->promise.set_value(io::VideoData());
            timeline->cancelRequests({first.id, pending.id, latest.id}); // idempotent, including completed IDs.
        }
        for (const bool reverse : {false, true})
        {
            auto timeline = make();
            auto gate = timeline->getVideo(otime::RationalTime(10, 30));
            auto held = waitHeld(plugin->reader);
            std::vector<timeline::VideoRequest> requests;
            for (int frame : {20, 70, 80}) requests.push_back(timeline->getVideo(otime::RationalTime(frame, 30)));
            timeline->setRequestPriority(otime::RationalTime(75, 30), reverse);
            held->promise.set_value(io::VideoData());
            get(gate.future);
            for (auto& request : requests) get(request.future);
            const auto expected = reverse ? std::vector<int>({10, 70, 20, 80}) : std::vector<int>({10, 80, 20, 70});
            check(plugin->reader->getOrder() == expected, "playhead priority was ignored");
        }
        io->removePlugin(plugin);
        std::cout << "PASS pending/in-flight cancellation, selective tokens, slot release, request priority\n";
    }

#if defined(TLRENDER_FFMPEG)
    void sameImage(const std::shared_ptr<image::Image>& a, const std::shared_ptr<image::Image>& b)
    {
        check(a && b, "missing decoded image");
        check(a->getInfo() == b->getInfo(), "image format changed");
        check(a->getData() && b->getData(), "test requires contiguous RGB data");
        check(a->getDataByteCount() == b->getDataByteCount(), "image size changed");
        check(!std::memcmp(a->getData(), b->getData(), a->getDataByteCount()), "displayed pixels do not match the target frame");
    }

    void movie(const std::shared_ptr<system::Context>& context, const std::string& filename)
    {
        io::Options rgb{{"FFmpeg/YUVToRGBConversion", "1"}};
        auto read = ffmpeg::VideoRead::create(file::Path(filename), rgb, context->getLogSystem());
        auto infoFuture = read->getInfo();
        const auto info = get(infoFuture);
        const double rate = info.videoTime->duration().rate();
        const auto start = info.videoTime->start_time();
        auto reference = ffmpeg::VideoRead::create(file::Path(filename), rgb, context->getLogSystem());
        auto referenceInfo = reference->getInfo();
        get(referenceInfo);
        std::vector<std::shared_ptr<io::CancellationToken> > tokens;
        std::vector<std::future<io::VideoData> > old;
        for (int frame : {741, 372, 783, 249, 516, 681})
        {
            tokens.push_back(std::make_shared<io::CancellationToken>());
            old.push_back(read->readVideo(start + otime::RationalTime(frame, rate), {}, tokens.back()));
        }
        auto knownInfo = read->getInfo();
        check(knownInfo.wait_for(0ms) == std::future_status::ready, "known info queued behind decoding");
        for (const auto& token : tokens) token->cancel();
        auto target = read->readVideo(start + otime::RationalTime(249, rate));
        auto expected = reference->readVideo(start + otime::RationalTime(249, rate));
        sameImage(get(target).image, get(expected).image);
        for (auto& future : old) get(future);

        auto audioRead = ffmpeg::AudioRead::create(file::Path(filename), rgb, context->getLogSystem());
        auto audioInfoFuture = audioRead->getInfo();
        const auto audioInfo = get(audioInfoFuture);
        if (audioInfo.audio.isValid())
        {
            auto audioReference = ffmpeg::AudioRead::create(file::Path(filename), rgb, context->getLogSystem());
            auto audioReferenceInfo = audioReference->getInfo();
            get(audioReferenceInfo);
            std::vector<std::future<io::AudioData> > oldAudio;
            tokens.clear();
            for (int second : {24, 12, 26, 8})
            {
                tokens.push_back(std::make_shared<io::CancellationToken>());
                oldAudio.push_back(audioRead->readAudio(
                    otime::TimeRange(
                        otime::RationalTime(second, 1).rescaled_to(audioInfo.audio.sampleRate),
                        otime::RationalTime(1, 1).rescaled_to(audioInfo.audio.sampleRate)), {}, tokens.back()));
            }
            for (const auto& token : tokens) token->cancel();
            const auto range = otime::TimeRange(
                otime::RationalTime(2, 1).rescaled_to(audioInfo.audio.sampleRate),
                otime::RationalTime(1, 1).rescaled_to(audioInfo.audio.sampleRate));
            auto actualAudio = audioRead->readAudio(range);
            auto expectedAudio = audioReference->readAudio(range);
            const auto actual = get(actualAudio);
            const auto expected = get(expectedAudio);
            check(actual.audio && expected.audio, "audio missing after cancellation");
            check(actual.time.strictly_equal(range.start_time()), "wrong audio position after cancellation");
            check(actual.audio->getByteCount() == expected.audio->getByteCount(), "audio length changed");
            check(!std::memcmp(actual.audio->getData(), expected.audio->getData(), actual.audio->getByteCount()),
                  "audio samples changed after cancellation");
            for (auto& future : oldAudio) get(future);
        }

        timeline::Options timelineOptions;
        timelineOptions.ioOptions = rgb;
        auto timeline = timeline::Timeline::create(context, filename, timelineOptions);
        timeline::PlayerOptions options;
        options.playback = timeline::Playback::Stop;
        auto player = timeline::Player::create(timeline, context, options);
        player->setMute(true);
        auto tick = [&] { context->tick(); player->tick(); std::this_thread::sleep_for(2ms); };
        auto frame = [&](int n)
        {
            const auto time = start + otime::RationalTime(n, rate);
            player->seek(time);
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (std::chrono::steady_clock::now() < deadline)
            {
                tick();
                const auto& video = player->getCurrentVideo();
                if (!video.empty() && video[0].time.strictly_equal(time) && !video[0].layers.empty() && video[0].layers[0].image)
                {
                    auto expected = reference->readVideo(time);
                    sameImage(video[0].layers[0].image, get(expected).image);
                    return;
                }
            }
            throw std::runtime_error("player did not deliver exact target frame");
        };
        for (int n : {741, 372, 783, 249, 516, 681}) frame(n);
        for (int n = 682; n <= 697; ++n) frame(n);
        for (int n = 696; n >= 681; --n) frame(n);
        for (int n = 682; n <= 690; ++n) frame(n);
        // Continuous seeks discard obsolete positions; only the final target is awaited.
        for (int n : {741, 372, 783, 249, 516}) { player->seek(start + otime::RationalTime(n, rate)); tick(); }
        frame(681);
        player->setPlayback(timeline::Playback::Forward);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        bool advanced = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            tick();
            const auto& video = player->getCurrentVideo();
            if (!video.empty() && !video[0].layers.empty() && video[0].layers[0].image &&
                video[0].time > start + otime::RationalTime(684, rate))
            {
                auto expected = reference->readVideo(video[0].time);
                sameImage(video[0].layers[0].image, get(expected).image);
                advanced = true;
                break;
            }
        }
        check(advanced, "playback failed to resume after seeks");
        player->setPlayback(timeline::Playback::Stop);
        frame(300);
        player->setPlayback(timeline::Playback::Reverse);
        const auto reverseDeadline = std::chrono::steady_clock::now() + 15s;
        bool reversed = false;
        while (std::chrono::steady_clock::now() < reverseDeadline)
        {
            tick();
            const auto& video = player->getCurrentVideo();
            if (!video.empty() && !video[0].layers.empty() && video[0].layers[0].image &&
                video[0].time < start + otime::RationalTime(297, rate))
            {
                auto expected = reference->readVideo(video[0].time);
                sameImage(video[0].layers[0].image, get(expected).image);
                reversed = true;
                break;
            }
        }
        check(reversed, "reverse playback did not deliver earlier images");
        player->setPlayback(timeline::Playback::Stop);
        for (int n : {0, 899, 0, 899}) frame(n);
        std::cout << "PASS FFmpeg cancellation, exact RGB pixels, seeks, forward/backward stepping, resume/pause: " << filename << '\n';
    }
#endif
}

int main(int argc, char** argv)
{
    try
    {
        auto context = system::Context::create();
        timeline::init(context);
        auto io = context->getSystem<io::ReadSystem>();

        scheduling(context, io);
#if defined(TLRENDER_FFMPEG)
        for (int i = 1; i < argc; ++i) movie(context, argv[i]);
#endif
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
