#include "key_pick.h"

#include "eye_state.h"
#include "menu_command.h"
#include "running_video.h"
#include "see_through.h"
#include "vr_input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

bool KeyPick::handle(const MenuCommand& c, const RunningVideo& running) {
  switch (c.type) {
    case MenuCommand::PickKeyColour:
      // Where the key is used: passthrough on a surround video, without its own mask.
      if (running.video.passthrough && running.video.greenScreen.on && !running.currentFile.empty() &&
          running.format.projection != Projection::Flat &&
          !(packedMask(running.currentFile, running.video) && maskFormat(running.format))) {
        for_ = running.currentFile;
        note_ = KeyPickNote::None;
        std::printf("Background key: pick a colour\n");
      }
      return true;
    case MenuCommand::CancelKeyPick:
      end();
      return true;
    default: return false;
  }
}

void KeyPick::end() {
  picker_.cancel();
  for_.clear();
  wanted_ = false;
  note_ = KeyPickNote::None;
}

void KeyPick::update(const ControllerState& input, const Frame& frame, const RunningVideo& running) {
  // The trigger away from the menu reads it where that hand's laser dot is
  // seen, as the scene shows the left eye's picture there (from that eye,
  // through the zoom, into the picture's axes, and onto its stereo
  // correction's sphere, as surround.frag). The dot is where the laser
  // draws it.
  if (!for_.empty() &&
      (for_ != running.currentFile || !frame.menuOpen || !running.video.passthrough ||
       !running.video.greenScreen.on || running.format.projection == Projection::Flat ||
       (packedMask(running.currentFile, running.video) && maskFormat(running.format))))
    end();
  const bool picking = !for_.empty();
  // Not while the room is peeked at: the video isn't seen.
  for (int h = 0; h < 2 && picking && !wanted_ && !picker_.busy() && frame.headValid &&
                  !frame.peeking;
       ++h) {
    const HandState& t = input.hands[h];
    if (h != frame.activeHand || !t.selectPressed || !t.poseValid || frame.onMenu[h]) continue;
    const EyeMatrices& eye = *frame.leftEye;
    const Mat4 view = rotation(eye.orientation);
    const Vec3 dotAt = t.aimPosition + t.pointDir * frame.laserLength;
    Vec3 d = transformDir(transposedRotation(view), normalize(dotAt - eye.position));
    if (d.z < 0) d = normalize(Vec3{d.x / running.screen.zoom, d.y / running.screen.zoom, d.z});
    Vec3 inPicture =
        transformDir(transposedRotation(running.screen.rotation()), transformDir(view, d));
    const EyeCorrection& correction = *frame.correction;
    if (correction.on && correction.distance > 0) {
      const Vec3 viewer = correction.viewer - correction.camera;
      const float along = dot(viewer, inPicture);
      const float reach = along * along - dot(viewer, viewer) +
                          correction.distance * correction.distance;
      inPicture = normalize(viewer + inPicture * (std::sqrt(std::max(reach, 0.0f)) - along));
    }
    wanted_ = KeyPicker::pointOf(running.format, frame.frameAspect, running.flipVideo, inPicture,
                                 uv_, part_);
    break;
  }
}

void KeyPick::read(bool unreadable, GLuint texture, bool tvGamma,
                   const std::function<bool()>& prepare, DepthPatchDrawer* drawer) {
  const KeyPicker::Read read =
      unreadable ? KeyPicker::Read::Unavailable
                 : picker_.read(uv_, part_, texture, tvGamma, prepare, drawer);
  if (read != KeyPicker::Read::Later) wanted_ = false;
  if (read == KeyPicker::Read::Unavailable) {
    note_ = KeyPickNote::Unreadable;
    std::printf("Background key: this picture can't be read\n");
  }
}

void KeyPick::takeResult(RunningVideo& running) {
  KeyColour picked;
  switch (picker_.result(picked)) {
    case KeyPicker::Result::Picked:
      running.video.greenScreen.source = KeySource::Picked;
      running.video.greenScreen.picked = picked;
      running.storeVideoSoon(VideoSettings::kGreenScreen);
      end();
      std::printf("Background key: picked %d, %d, %d\n", picked.r, picked.g, picked.b);
      break;
    case KeyPicker::Result::Uneven:
      note_ = KeyPickNote::Uneven;
      std::printf("Background key: not one colour there\n");
      break;
    case KeyPicker::Result::Failed:
      std::printf("Background key: the colour wasn't read back\n");
      break;
    case KeyPicker::Result::None: break;
  }
}

void KeyPick::fill(MenuView& view) const {
  view.keyPicking = !for_.empty();
  view.keyPickNote = note_;
}
