/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/modules/video_render//video_render_frames.h"

#include <assert.h>

#include "webrtc/common_video/interface/texture_video_frame.h"
#include "webrtc/modules/interface/module_common_types.h"
#include "webrtc/system_wrappers/interface/tick_util.h"
#include "webrtc/system_wrappers/interface/trace.h"

namespace webrtc {

const int32_t KEventMaxWaitTimeMs = 200;
const uint32_t kMinRenderDelayMs = 10;
const uint32_t kMaxRenderDelayMs= 500;

VideoRenderFrames::VideoRenderFrames()
    : incoming_frames_(),
      render_delay_ms_(10) {
}

VideoRenderFrames::~VideoRenderFrames() {
  ReleaseAllFrames();
}

int32_t VideoRenderFrames::AddFrame(I420VideoFrame* new_frame) {
  const int64_t time_now = TickTime::MillisecondTimestamp();

  // Drop old frames only when there are other frames in the queue, otherwise, a
  // really slow system never renders any frames.
  if (!incoming_frames_.Empty() &&
      new_frame->render_time_ms() + KOldRenderTimestampMS < time_now) {
    WEBRTC_TRACE(kTraceWarning,
                 kTraceVideoRenderer,
                 -1,
                 "%s: too old frame, timestamp=%u.",
                 __FUNCTION__,
                 new_frame->timestamp());
    return -1;
  }

  if (new_frame->render_time_ms() > time_now + KFutureRenderTimestampMS) {
    WEBRTC_TRACE(kTraceWarning, kTraceVideoRenderer, -1,
                 "%s: frame too long into the future, timestamp=%u.",
                 __FUNCTION__, new_frame->timestamp());
    return -1;
  }

  if (new_frame->native_handle() != NULL) {
    incoming_frames_.PushBack(new TextureVideoFrame(
        static_cast<NativeHandle*>(new_frame->native_handle()),
        new_frame->width(),
        new_frame->height(),
        new_frame->timestamp(),
        new_frame->render_time_ms()));
    return incoming_frames_.GetSize();
  }

  // Get an empty frame
  I420VideoFrame* frame_to_add = NULL;
  if (!empty_frames_.Empty()) {
    ListItem* item = empty_frames_.First();
    if (item) {
      frame_to_add = static_cast<I420VideoFrame*>(item->GetItem());
      empty_frames_.Erase(item);
    }
  }
  if (!frame_to_add) {
    if (empty_frames_.GetSize() + incoming_frames_.GetSize() >
        KMaxNumberOfFrames) {
      // Already allocated too many frames.
      WEBRTC_TRACE(kTraceWarning, kTraceVideoRenderer,
                   -1, "%s: too many frames, timestamp=%u, limit=%d",
                   __FUNCTION__, new_frame->timestamp(), KMaxNumberOfFrames);
      return -1;
    }

    // Allocate new memory.
    WEBRTC_TRACE(kTraceMemory, kTraceVideoRenderer, -1,
                 "%s: allocating buffer %d", __FUNCTION__,
                 empty_frames_.GetSize() + incoming_frames_.GetSize());

    frame_to_add = new I420VideoFrame();
    if (!frame_to_add) {
      WEBRTC_TRACE(kTraceError, kTraceVideoRenderer, -1,
                   "%s: could not create new frame for", __FUNCTION__);
      return -1;
    }
  }

  frame_to_add->CreateEmptyFrame(new_frame->width(), new_frame->height(),
                                 new_frame->stride(kYPlane),
                                 new_frame->stride(kUPlane),
                                 new_frame->stride(kVPlane));
  // TODO(mflodman) Change this!
  // Remove const ness. Copying will be costly.
  frame_to_add->SwapFrame(new_frame);
  incoming_frames_.PushBack(frame_to_add);

  return incoming_frames_.GetSize();
}

I420VideoFrame* VideoRenderFrames::FrameToRender() {
  I420VideoFrame* render_frame = NULL;
  while (!incoming_frames_.Empty()) {
    ListItem* item = incoming_frames_.First();
    if (item) {
      I420VideoFrame* oldest_frame_in_list =
          static_cast<I420VideoFrame*>(item->GetItem());
      if (oldest_frame_in_list->render_time_ms() <=
          TickTime::MillisecondTimestamp() + render_delay_ms_) {
        // This is the oldest one so far and it's OK to render.
        if (render_frame) {
          // This one is older than the newly found frame, remove this one.
          ReturnFrame(render_frame);
        }
        render_frame = oldest_frame_in_list;
        incoming_frames_.Erase(item);
      } else {
        // We can't release this one yet, we're done here.
        break;
      }
    } else {
      assert(false);
    }
  }
  return render_frame;
}

int32_t VideoRenderFrames::ReturnFrame(I420VideoFrame* old_frame) {
  // No need to reuse texture frames because they do not allocate memory.
  if (old_frame->native_handle() == NULL) {
    old_frame->ResetSize();
    old_frame->set_timestamp(0);
    old_frame->set_render_time_ms(0);
    empty_frames_.PushBack(old_frame);
  } else {
    delete old_frame;
  }
  return 0;
}

int32_t VideoRenderFrames::ReleaseAllFrames() {
  while (!incoming_frames_.Empty()) {
    ListItem* item = incoming_frames_.First();
    if (item) {
      I420VideoFrame* frame = static_cast<I420VideoFrame*>(item->GetItem());
      assert(frame != NULL);
      delete frame;
    }
    incoming_frames_.Erase(item);
  }
  while (!empty_frames_.Empty()) {
    ListItem* item = empty_frames_.First();
    if (item) {
      I420VideoFrame* frame = static_cast<I420VideoFrame*>(item->GetItem());
      assert(frame != NULL);
      delete frame;
    }
    empty_frames_.Erase(item);
  }
  return 0;
}

uint32_t VideoRenderFrames::TimeToNextFrameRelease() {
  int64_t time_to_release = 0;
  ListItem* item = incoming_frames_.First();
  if (item) {
    I420VideoFrame* oldest_frame =
        static_cast<I420VideoFrame*>(item->GetItem());
    time_to_release = oldest_frame->render_time_ms() - render_delay_ms_
                      - TickTime::MillisecondTimestamp();
    if (time_to_release < 0) {
      time_to_release = 0;
    }
  } else {
    time_to_release = KEventMaxWaitTimeMs;
  }
  return static_cast<uint32_t>(time_to_release);
}

int32_t VideoRenderFrames::SetRenderDelay(
    const uint32_t render_delay) {
  if (render_delay < kMinRenderDelayMs ||
      render_delay > kMaxRenderDelayMs) {
    WEBRTC_TRACE(kTraceWarning, kTraceVideoRenderer,
                 -1, "%s(%d): Invalid argument.", __FUNCTION__,
                 render_delay);
    return -1;
  }

  render_delay_ms_ = render_delay;
  return 0;
}

}  // namespace webrtc
