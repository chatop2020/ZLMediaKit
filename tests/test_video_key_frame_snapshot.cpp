/*
 * 测试 GOP 快照只保留视频配置帧和最新关键帧，并保证引用在缓存轮换后有效。
 * Test that a GOP snapshot keeps only video configuration frames and the latest
 * key frame, with references remaining valid after source-cache rotation.
 */

#include <iostream>
#include "Common/VideoKeyFrameSnapshot.h"

using namespace mediakit;

// 构造可控的缓存帧，便于覆盖音频、配置帧及新旧关键帧。
// Construct controllable cached frames for audio, configuration and key-frame cases.
class SnapshotTestFrame : public FrameImp {
public:
    /// 是否为关键帧 / Whether this is a key frame.
    bool is_key = false;
    /// 是否为配置帧 / Whether this is a configuration frame.
    bool is_config = false;
    /// 回报关键帧标记 / Report the key-frame flag.
    bool keyFrame() const override { return is_key; }
    /// 回报配置帧标记 / Report the configuration-frame flag.
    bool configFrame() const override { return is_config; }
};

/**
 * 创建指定类型和大小的测试帧。
 * Create a test frame with the requested codec and payload size.
 * @param codec 编码类型 / Codec type.
 * @param bytes 有效载荷字节数 / Payload byte count.
 * @param pts 显示时间戳 / Presentation timestamp.
 * @param key 是否为关键帧 / Whether it is a key frame.
 * @param config 是否为配置帧 / Whether it is a configuration frame.
 */
static SnapshotTestFrame::Ptr makeFrame(CodecId codec, size_t bytes, uint64_t pts, bool key, bool config) {
    auto frame = FrameImp::create<SnapshotTestFrame>();
    frame->_codec_id = codec;
    frame->_pts = pts;
    frame->_dts = pts;
    frame->_buffer = std::string(bytes, 'x');
    frame->is_key = key;
    frame->is_config = config;
    return frame;
}

/**
 * 验证最新视频关键帧、总字节上限和共享引用生命周期。
 * Verify newest-video selection, the combined byte limit and shared-reference lifetime.
 */
int main() {
    VideoKeyFrameSnapshotBuilder builder(32);
    auto config = makeFrame(CodecH264, 4, 1, false, true);
    auto old_key = makeFrame(CodecH264, 10, 2, true, false);
    auto latest_key = makeFrame(CodecH264, 12, 3, true, false);
    auto latest_slice = makeFrame(CodecH264, 6, 3, false, false);
    builder.inputFrame(makeFrame(CodecAAC, 8, 2, false, false));
    builder.inputFrame(config);
    builder.inputFrame(old_key);
    builder.inputFrame(makeFrame(CodecH264, 6, 2, false, false));
    builder.inputFrame(latest_key);
    builder.inputFrame(makeFrame(CodecAAC, 5, 3, false, false));
    builder.inputFrame(latest_slice);
    builder.inputFrame(makeFrame(CodecH264, 7, 4, false, false));

    VideoKeyFrameSnapshot snapshot;
    if (!builder.take(snapshot) || snapshot.codec != CodecH264 || snapshot.pts != 3
        || snapshot.bytes != 22 || snapshot.frames.size() != 3
        || snapshot.frames[1] != latest_key || snapshot.frames.back() != latest_slice
        || snapshot.frames.front() != config) {
        std::cerr << "latest video key-frame snapshot failed" << std::endl;
        return 1;
    }

    // 模拟缓存轮换：释放源引用，快照仍持有自身引用。
    // Simulate cache rotation: release source references while the snapshot retains its own.
    config.reset(); old_key.reset(); latest_key.reset(); latest_slice.reset();
    if (!snapshot.frames.front() || snapshot.frames.front()->size() != 4
        || !snapshot.frames.back() || snapshot.frames.back()->size() != 6) return 2;

    VideoKeyFrameSnapshotBuilder overflow(16);
    overflow.inputFrame(makeFrame(CodecH264, 4, 4, false, true));
    overflow.inputFrame(makeFrame(CodecH264, 8, 5, true, false));
    overflow.inputFrame(makeFrame(CodecH264, 12, 5, false, false));
    VideoKeyFrameSnapshot rejected;
    if (overflow.take(rejected)) return 3;

    VideoKeyFrameSnapshotBuilder audio_only;
    audio_only.inputFrame(makeFrame(CodecAAC, 8, 7, true, false));
    if (audio_only.take(rejected)) return 4;
    std::cout << "video key-frame snapshot tests passed" << std::endl;
    return 0;
}
