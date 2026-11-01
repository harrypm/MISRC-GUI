# Monitor video — stage 2: decoded-frame handoff

Branch: \`feature/monitor-h264-video-20261101\`.

## Implemented

- CVBS/Tape decoder's \`complete_field_pll()\` can synchronously invoke an optional RGB24 frame sink after finishing the decoded frame. Install the callback only while decoding is stopped; its pixel pointer is valid only during the callback.
- VHS RF preview worker can call an optional RGBA32 frame sink after frame publication. Callback pointer is installed under the worker's frame mutex and runs on the preview worker, not on the capture thread. Its context must remain alive until callbacks are stopped and the worker joined.
- A separately testable, single-latest-frame \`gui_monitor_video_framebox\` owns a fixed buffer allocated at initialization. Its producer and consumer use a non-spinning try-lock. If either is occupied, the new preview is dropped or the consumer retries later. Never block recording/capture.
- The framebox can be bound using \`gui_monitor_video_framebox_cvbs_sink\` or \`gui_monitor_video_framebox_rf_sink\`. It copies only RGB/RGBA pixels, preserving width, height, format, monotonic timestamp and generation. It neither encodes video nor touches sockets or UI.

## Deliberate boundaries

**No live headless video yet:** the server does not initialize independent decoders, create a framebox/subscription, bind sinks, encode H.264, offer \`/monitor/video\`, or render remote video. The first stage framing and RGB-to-I420 converter remain unused in production. Hook registration is opt-in; existing behavior remains unchanged without a sink.

Before enabling headless decode, route captured samples through an independently scheduled decoder input queue with a strict time/memory budget. Keep capture/recording unaffected by network congestion. Ensure callback contexts outlive their decoders, and stop/join all workers before destroying frameboxes. PAL/NTSC fields can arrive more often than target display FPS: encoder-side pacing or explicit drop is needed. H.264 dimensions must be even; crop/scale odd full-frame geometry using the stage 1 I420 converter. Never assume an unchanged field is a new complete interlaced picture.

## Standalone test

Run \`sh scripts/test-monitor-video-framebox.sh\` on a C11 compiler supporting threads. Tests cover bounds, RGB/RGBA adapters, generation, snapshots and concurrent publication/consumption. GCC/Clang address+undefined sanitizer runs are advised. Full application builds and actual capture tests remain pending.
