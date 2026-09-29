/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 * 源码许可遵循仓库 LICENSE 中的 MIT-like 条款。
 * Use of this source code is governed by the MIT-like license in the LICENSE file.
 */

#ifndef ZLMEDIAKIT_VIDEO_KEY_FRAME_SNAPSHOT_H
#define ZLMEDIAKIT_VIDEO_KEY_FRAME_SNAPSHOT_H

#include <deque>
#include <vector>
#include "Extension/Frame.h"

namespace mediakit {

/**
 * 一次视频关键帧快照，仅持有配置帧与最新关键帧的共享引用，不复制帧数据。
 * A video key-frame snapshot holding shared references to configuration frames
 * and the latest key frame, without copying frame payloads.
 */
struct VideoKeyFrameSnapshot {
    /// 视频编码类型 / Video codec type.
    CodecId codec = CodecInvalid;
    /// 关键帧解码时间戳，毫秒 / Key-frame decoding timestamp in milliseconds.
    uint64_t dts = 0;
    /// 关键帧显示时间戳，毫秒 / Key-frame presentation timestamp in milliseconds.
    uint64_t pts = 0;
    /// 全部选中帧的字节数 / Total bytes of the selected frames.
    size_t bytes = 0;
    /// 配置帧在前、关键帧在后 / Configuration frames followed by the key frame.
    std::vector<Frame::Ptr> frames;
};

/**
 * 从一个或多个缓存 GOP 中选择最新的视频关键帧，忽略所有音频帧。
 * Select the latest video key frame from cached GOPs while ignoring audio.
 * 该对象只收集引用并限制总字节；实际数据复制应在媒体线程之外完成。
 * This object only collects references and enforces a byte limit; copy the
 * payload outside the media thread.
 */
class VideoKeyFrameSnapshotBuilder {
public:
    /**
     * 创建有总字节限制的选择器。
     * Create a selector with a combined payload limit.
     * @param max_bytes 快照最大字节数 / Maximum bytes in one snapshot.
     */
    explicit VideoKeyFrameSnapshotBuilder(size_t max_bytes = 2 * 1024 * 1024)
        : _max_bytes(max_bytes) {}

    /**
     * 按缓存顺序输入一帧；仅保留少量配置帧和最新视频关键帧的引用。
     * Feed one frame in cache order; retain only a few configuration-frame
     * references and the latest video key-frame reference.
     * @param frame 当前缓存帧，允许为空 / Current cached frame; null is allowed.
     */
    void inputFrame(const Frame::Ptr &frame) {
        // 音频不会进入视频快照。/ Audio never enters a video snapshot.
        if (!frame || frame->getTrackType() != TrackVideo) return;
        if (frame->configFrame()) {
            // 最多保留八个配置帧引用，避免异常流无限占用内存。
            // Keep at most eight configuration references to bound memory on malformed streams.
            _configs.emplace_back(frame);
            if (_configs.size() > 8) _configs.pop_front();
            return;
        }
        if (!frame->keyFrame()) return;

        // 每次关键帧到达均替换候选快照，使最终结果属于最新 GOP。
        // Replace the candidate on each key frame so the result belongs to the latest GOP.
        VideoKeyFrameSnapshot candidate;
        candidate.codec = frame->getCodecId();
        candidate.dts = frame->dts();
        candidate.pts = frame->pts();
        if (frame->size() == 0 || frame->size() > _max_bytes) {
            // 超限时清除旧候选，不能退回上一 GOP 并冒充最新画面。
            // Clear an older candidate on overflow instead of presenting it as the latest GOP.
            _latest = VideoKeyFrameSnapshot{};
            return;
        }
        candidate.bytes = frame->size();
        for (const auto &config : _configs) {
            if (config->getCodecId() != candidate.codec) continue;
            if (config->size() > _max_bytes - candidate.bytes) {
                _latest = VideoKeyFrameSnapshot{};
                return;
            }
            candidate.bytes += config->size();
            candidate.frames.emplace_back(config);
        }
        candidate.frames.emplace_back(frame);
        _latest = std::move(candidate);
    }

    /**
     * 移出当前最新快照，调用者在媒体线程外复制有效载荷。
     * Move out the latest snapshot; the caller copies payloads outside the media thread.
     * @param result 输出快照 / Output snapshot.
     * @return 是否找到符合上限的视频关键帧 / Whether a bounded video key frame was found.
     */
    bool take(VideoKeyFrameSnapshot &result) {
        if (_latest.frames.empty()) return false;
        result = std::move(_latest);
        return true;
    }

private:
    /// 本次请求的总字节上限 / Combined byte limit for this request.
    size_t _max_bytes;
    /// 最多八个最近配置帧引用 / Up to eight recent configuration-frame references.
    std::deque<Frame::Ptr> _configs;
    /// 最新完整关键帧候选 / Latest complete key-frame candidate.
    VideoKeyFrameSnapshot _latest;
};

} // namespace mediakit

#endif // ZLMEDIAKIT_VIDEO_KEY_FRAME_SNAPSHOT_H
