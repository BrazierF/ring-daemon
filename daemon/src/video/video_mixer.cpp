/*
 *  Copyright (C) 2013 Savoir-Faire Linux Inc.
 *
 *  Author: Guillaume Roguez <Guillaume.Roguez@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *  Additional permission under GNU GPL version 3 section 7:
 *
 *  If you modify this program, or any covered work, by linking or
 *  combining it with the OpenSSL project's OpenSSL library (or a
 *  modified version of that library), containing parts covered by the
 *  terms of the OpenSSL or SSLeay licenses, Savoir-Faire Linux Inc.
 *  grants you additional permission to convey the resulting work.
 *  Corresponding Source for a non-source form of such a combination
 *  shall include the source code for the parts of OpenSSL used as well
 *  as that of the covered work.
 */

#include "libav_deps.h"
#include "video_mixer.h"
#include "check.h"
#include "client/video_controls.h"
#include "manager.h"
#include "logger.h"

#include <cmath>

namespace sfl_video {

VideoMixer::VideoMixer(const std::string &id) :
    VideoGenerator::VideoGenerator()
    , id_(id)
    , width_(0)
    , height_(0)
    , sources_()
    , mutex_()
    , sink_(id+"_MX")
{
    auto videoCtrl = Manager::instance().getVideoControls();
    if (!videoCtrl->hasPreviewStarted()) {
        videoCtrl->startPreview();
        MYSLEEP(1);
    }

    // Local video camera is always attached
    videoCtrl->getVideoPreview()->attach(this);
}

VideoMixer::~VideoMixer()
{
    stop_sink();

    auto videoCtrl = Manager::instance().getVideoControls();
    videoCtrl->getVideoPreview()->detach(this);
}

void VideoMixer::attached(Observable<VideoFrameSP>* ob)
{
    std::lock_guard<std::mutex> lk(mutex_);
    sources_.push_back(ob);
}

void VideoMixer::detached(Observable<VideoFrameSP>* ob)
{
    std::lock_guard<std::mutex> lk(mutex_);
    sources_.remove(ob);
}

void VideoMixer::update(Observable<VideoFrameSP>* ob, VideoFrameSP& frame_p)
{
    std::lock_guard<std::mutex> lk(mutex_);
    int i=0;
    for (auto x : sources_) {
        if (x == ob) break;
        i++;
    }
    render_frame(*frame_p, i);
}

void VideoMixer::render_frame(VideoFrame& input, const int index)
{
    VideoScaler scaler;
    VideoFrame scaled_input;

    if (!width_ or !height_)
        return;

    VideoFrame &output = getNewFrame();

    if (!output.allocBuffer(width_, height_, VIDEO_PIXFMT_YUV420P)) {
        ERROR("VideoFrame::allocBuffer() failed");
        return;
    }

    VideoFrameSP previous_p=obtainLastFrame();
    if (previous_p)
        previous_p->copy(output);
    previous_p.reset();

    const int n=sources_.size();
    const int zoom=ceil(sqrt(n));
    const int cell_width=width_ / zoom;
    const int cell_height=height_ / zoom;

    if (!scaled_input.allocBuffer(cell_width, cell_height,
                                  VIDEO_PIXFMT_YUV420P)) {
        ERROR("VideoFrame::allocBuffer() failed");
        return;
    }

    int xoff = (index % zoom) * cell_width;
    int yoff = (index / zoom) * cell_height;

    scaler.scale(input, scaled_input);
    output.blit(scaled_input, xoff, yoff);

    publishFrame();
}

void VideoMixer::setDimensions(int width, int height)
{
    std::lock_guard<std::mutex> lk(mutex_);
    width_ = width;
    height_ = height;

    // cleanup the previous frame to have a nice copy in rendering method
    VideoFrameSP previous_p=obtainLastFrame();
    if (previous_p)
        previous_p->clear();

    stop_sink();
    start_sink();
}

void VideoMixer::start_sink()
{
    if (sink_.start()) {
        if (this->attach(&sink_)) {
            Manager::instance().getVideoControls()->startedDecoding(id_, sink_.openedName(), width_, height_);
            DEBUG("MX: shm sink <%s> started: size = %dx%d",
                  sink_.openedName().c_str(), width_, height_);
        }
    } else
        WARN("MX: sink startup failed");
}

void VideoMixer::stop_sink()
{
    if (this->detach(&sink_)) {
        Manager::instance().getVideoControls()->stoppedDecoding(id_, sink_.openedName());
        sink_.stop();
    }
}

int VideoMixer::getWidth() const
{ return width_; }

int VideoMixer::getHeight() const
{ return height_; }

int VideoMixer::getPixelFormat() const
{ return VIDEO_PIXFMT_YUV420P; }

} // end namespace sfl_video