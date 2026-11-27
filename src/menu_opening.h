#pragma once

// How the menu opens or closes this frame, outside first-run setup: at
// start (--open-menu), in the file browser at start without a video, at a
// video's end, or by the menu button or Tab, which first cancels picking
// the key's colour.
enum class MenuOpening {
  Leave,          // as it is
  StartPage,      // open on --open-menu's page
  StartBrowser,   // open in the file browser
  AtVideoEnd,     // open on the list the video was played from
  CancelKeyPick,  // the button ends picking the key's colour; the menu stays
  Toggle,         // open or close
};

struct MenuOpeningInput {
  bool headValid = false;
  bool startPage = false;      // --open-menu's page waits to be shown
  bool startBrowser = false;   // no video at start: the file browser waits
  bool videoEnded = false;     // a video ended, for the menu (PlaybackSession::menuAtEnd)
  bool pressed = false;        // the menu button, or the ToggleMenu command
  bool keyPicking = false;
};

// The first that applies, in that order.
inline MenuOpening menuOpening(const MenuOpeningInput& in) {
  if (in.startPage && in.headValid) return MenuOpening::StartPage;
  if (in.startBrowser && in.headValid) return MenuOpening::StartBrowser;
  if (in.videoEnded && in.headValid) return MenuOpening::AtVideoEnd;
  if (in.pressed && in.keyPicking) return MenuOpening::CancelKeyPick;
  if (in.pressed) return MenuOpening::Toggle;
  return MenuOpening::Leave;
}
