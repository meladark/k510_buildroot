#pragma once

#include <stdint.h>

class Photos;
class VideoRecorder;

// Trigger mode: the AI camera watches for the configured classes; when one is
// seen on trigger_confirm consecutive AI frames, a photo (with the detection
// drawn) is taken and the other camera records trigger_video_s seconds. While
// that video runs nothing new fires; if the object is still there afterwards,
// the next video starts right away.
class Trigger {
public:
    void tick(Photos &photos, VideoRecorder &video);  // call often (main loop)

private:
    uint64_t last_frame_ = 0;
    int streak_ = 0;
};
