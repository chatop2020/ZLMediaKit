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
        if (frame->keyFrame()) {
            // 一个视频画面可能含多个 NAL；关键帧标记只保证首个切片。
            // A picture may contain multiple NAL units; the key flag may mark only its first slice.
            if (_collecting_picture && frame->getCodecId() == _candidate.codec
                && frame->pts() == _candidate.pts) {
                appendBounded(frame);
                return;
            }
            if (_collecting_picture && !_candidate.frames.empty()) _latest = std::move(_candidate);
            _candidate = VideoKeyFrameSnapshot{};
            _candidate.codec = frame->getCodecId();
            _candidate.dts = frame->dts();
            _candidate.pts = frame->pts();
            _collecting_picture = true;
            for (const auto &config : _configs) {
                if (config->getCodecId() == _candidate.codec && !appendBounded(config)) return;
            }
            appendBounded(frame);
            return;
        }
        if (!_collecting_picture) return;
        if (frame->getCodecId() != _candidate.codec || frame->pts() != _candidate.pts) {
            // 看到下一张视频画面的时间戳后，上一张的全部切片才确定已收齐。
            // Only the next picture's timestamp proves all slices of the prior picture arrived.
            _latest = std::move(_candidate);
            _collecting_picture = false;
            return;
        }
        // 追加同一 PTS 的后续视频切片，确保解码器收到完整画面。
        // Append subsequent video slices with the same PTS so the decoder sees a complete picture.
        appendBounded(frame);
    }

    /**
     * 移出最新已确认完整的快照，调用者在媒体线程外复制有效载荷。
     * Move out the latest confirmed-complete snapshot; the caller copies payloads outside the media thread.
     * @param result 输出快照 / Output snapshot.
     * @return 是否找到已确认完整且符合上限的视频关键画面 / Whether a confirmed-complete, bounded key picture was found.
     */
    bool take(VideoKeyFrameSnapshot &result) {
        if (_latest.frames.empty()) return false;
        result = std::move(_latest);
        return true;
    }

    /**
     * 复制已确认完整画面的共享引用，不消费缓存；供多个按需请求复用同一张图。
     * Copy shared references to the confirmed-complete picture without consuming it.
     * @param result 输出快照 / Output snapshot.
     * @param max_bytes 调用者允许的总字节数 / Caller payload limit.
     * @return 有完整且未超限的画面时为 true / True for a complete picture within the caller limit.
     */
    bool copyLatest(VideoKeyFrameSnapshot &result, size_t max_bytes) const {
        if (_latest.frames.empty() || _latest.bytes > max_bytes) return false;
        result = _latest;
        return true;
    }

private:
    /**
     * 有界追加一帧；超限即丢弃整张画面，绝不回退到上一关键帧。
     * Append one frame within limits; reject the whole picture on overflow
     * instead of falling back to a stale key frame.
     * @param frame 待追加视频帧 / Video frame to append.
     * @return 是否成功追加 / Whether it was appended.
     */
    bool appendBounded(const Frame::Ptr &frame) {
        if (frame->size() == 0 || frame->size() > _max_bytes - _candidate.bytes
            || _candidate.frames.size() >= 64) {
            _candidate = VideoKeyFrameSnapshot{};
            _latest = VideoKeyFrameSnapshot{};
            _collecting_picture = false;
            return false;
        }
        _candidate.bytes += frame->size();
        _candidate.frames.emplace_back(frame);
        return true;
    }

    /// 本次请求的总字节上限 / Combined byte limit for this request.
    size_t _max_bytes;
    /// 最多八个最近配置帧引用 / Up to eight recent configuration-frame references.
    std::deque<Frame::Ptr> _configs;
    /// 最新已确认完整的关键画面 / Latest confirmed-complete key picture.
    VideoKeyFrameSnapshot _latest;
    /// 正在接收同一 PTS 后续切片的候选画面 / Candidate awaiting later slices with the same PTS.
    VideoKeyFrameSnapshot _candidate;
    /// 是否继续收集当前关键画面的切片 / Whether the current key picture still accepts slices.
    bool _collecting_picture = false;
};

} // namespace mediakit

#endif // ZLMEDIAKIT_VIDEO_KEY_FRAME_SNAPSHOT_H
