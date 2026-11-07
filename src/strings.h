#pragma once

// English is the compiled source. Each strings_XX.cpp copies it, then assigns
// translations by member name (including array indices); omissions keep English
// and unknown members fail compilation. Keep printf conversions unchanged.
// Register the new table in strings.cpp and its source in CMakeLists.txt.
// strings() is a pointer lookup, from any thread; locale detection runs only when selecting a language.

#include <string>

// Stash's text (docs/stash.md): its Settings group, navigation section and
// Scenes page. printf formats where marked; %s for numbers written with
// thousands separators.
struct StashStrings {
  // Settings.
  const char* group;
  const char* use;
  const char* offHint;
  const char* setupHint;
  const char* server;
  const char* apiKey;
  const char* apiKeyNone;
  const char* apiKeySet;
  const char* folders;
  const char* covers;
  const char* titles;
  const char* malePerformers;
  const char* status;        // printf: scenes, listed here, matched, library videos
  const char* statusStreamed;  // ... with some from Stash's stream: scenes, here, streamed, matched, library videos
  const char* synced;        // printf: when, Stash's version
  const char* syncing;
  const char* loadingCopy;
  const char* notSynced;
  const char* syncNow;
  const char* justNow;
  const char* minutesAgo;    // printf: minutes
  const char* hoursAgo;      // printf: hours
  const char* daysAgo;       // printf: days
  const char* editTitle;
  const char* fieldNames[4];  // URL, API key, Stash's folder, this computer's
  const char* editHint;
  const char* save;
  // Navigation.
  const char* navHome;
  const char* navScenes;
  const char* navPerformers;
  const char* navStudios;
  const char* allPins;  // printf: pinned lists
  // The Home, Performers and Studios pages.
  const char* home;
  const char* performers;
  const char* studios;
  // The Scenes page.
  const char* scenes;
  const char* count;    // printf: scenes
  const char* countOf;  // printf: shown, all
  const char* filters;
  const char* tipFilters;
  const char* shuffle;
  const char* pin;
  const char* rename;
  const char* unpin;
  const char* formats[3];  // StashFormat
  const char* searchScenes;
  const char* sortFields[12];  // StashSort
  const char* tipTurnOrder;
  const char* undo;
  const char* clearAll;
  const char* noFilters;
  const char* moreChips;  // printf: chips not shown
  const char* loading;
  const char* noCatalog;
  const char* noScenes;
  const char* sections[8];  // performers, tags, studios, cast, looks, release date, format, history
  const char* allOf;
  const char* anyOf;
  const char* more[4];  // performers, tags, studios, years
  const char* women;
  const char* men;
  const char* womenCounts[3];  // StashWomen
  const char* menCounts[4];    // StashMen
  const char* hair;
  const char* cup;
  const char* breasts;
  const char* age;
  const char* cups[4];
  const char* breastKinds[2];
  const char* ages[4];
  const char* ethnicity;
  const char* country;
  const char* height;
  const char* eyeColour;
  const char* shape;
  const char* tattoos;
  const char* heights[4];      // StashHeight
  const char* shapes[5];       // StashShape, including the Unclassified filter status
  const char* tattooKinds[2];  // StashTattoos
  const char* ageInScenes;     // the Performers page's age heading
  const char* added;
  const char* addedSpans[4];  // last 7, 30, 90, 365 days
  const char* released;
  const char* resolution;
  const char* resolutions[10];  // StashResolution
  const char* duration;
  const char* durations[4];
  const char* frameRate;
  const char* frameRates[3];
  const char* codec;
  const char* codecs[3];  // StashCodec
  const char* fileSize;
  const char* fileSizes[4];  // StashFileSize
  const char* history[5];  // StashHistoryOption
  const char* nothingToNarrow;
  // Editing which of the rail's sections show, and their order.
  const char* editFilters;
  const char* filtersShown;
  const char* resetFilters;
  const char* doneFilters;
  const char* tipMoveUp;
  const char* tipMoveDown;
  const char* hairChip;      // printf: colour
  const char* cupChip;       // printf: cup
  const char* ageChip;       // printf: band
  const char* releasedChip;  // printf: year
  const char* without;       // printf: a left-out name, in a pinned list's name
  const char* back;
  const char* fullTitle;  // printf: what (tags, performers …), how many
  const char* byCount;
  const char* alphabetical;
  const char* find[4];  // performers, tags, studios, years
  // A named look's full list (hair, ethnicity, country, eye colour): its
  // link, its search, and on the Performers page its title and Back.
  const char* moreLooks[4];
  const char* findLooks[4];
  const char* fullTitlePerformers;  // printf: what, how many
  const char* backToPerformers;
  const char* pins;
  const char* noMatches;  // printf: the search words
  const char* columns[6];  // title, rating, plays, studio, released, length
  const char* tipExclude;
  const char* vr;
  const char* flat;
  // The Performers and performer pages.
  const char* performersCount;    // printf: performers
  const char* performersCountOf;  // printf: shown, all
  const char* searchPerformers;
  const char* performerSorts[5];  // StashPerformerSort
  const char* performerHistory[3];  // StashPerformerHistory
  const char* theirScenes;
  const char* tipTheirScenes;
  const char* noPerformers;
  const char* performerGone;
  const char* oneScene;
  const char* playsCount;   // printf: plays
  const char* yearsOld;     // printf: age
  const char* born;         // printf: date
  const char* eyes;         // printf: colour
  const char* centimeters;  // printf: height
  const char* alsoKnownAs;  // printf: aliases
  const char* activeSince;  // printf: year
  const char* activeFromTo;  // printf: from, to
  const char* scenesHere;
  const char* played;
  const char* averageOf;    // printf: rated scenes
  const char* noneRated;
  const char* lastPlayed;   // printf: when
  const char* neverPlayed;
  const char* oftenWith;
  const char* titleWith;    // printf: her name, the co-stars'
  const char* and_;         // between co-stars' names
  const char* tipCoStar;
  // The Studios page and a studio's.
  const char* studiosCount;    // printf: studios
  const char* studioSorts[5];  // your plays, scenes, newest release, name, your rating
  const char* onePlay;
  const char* newest;    // printf: date
  const char* newestIn;  // printf: date
  const char* partOf;    // printf: the studio Stash has it under
  const char* average;   // printf: stars
  const char* noStudios;
  const char* stars;  // printf: an average rating
  // The Home page.
  const char* shelves[4];     // new in Stash, top rated, top performers, random picks
  const char* shelfNotes[4];  // new in Stash: printf, added in the last 30 days
  const char* seeAll;         // printf: how many
  const char* pinFromScenes;
  const char* noPins;
  const char* searchStash;
  const char* tipUse;
  const char* tipFields[4];
  const char* tipSync;
  const char* tipPin;
  const char* tipShuffle;
  const char* tipFilter;
  const char* tipRemoveFilter;  // printf: filter name
  const char* tipMatch[2];  // any, all
  const char* tipJumpLetter;
  const char* tipFullSort[2];  // most first, alphabetical
  const char* tipYearSort;
  const char* yearOrder[2];  // the years' full list: newest first, oldest first
};

// Stash's write-back (docs/stash.md, "Write-back safety"): its switch in
// Settings and the one-off send. printf formats where marked; %s for
// numbers written with thousands separators.
struct StashWriteBackStrings {
  const char* toggle;
  const char* hint;
  const char* waiting;  // printf: ratings, plays
  const char* sent;     // printf: ratings, plays
  const char* sendAll;
  const char* title;
  const char* checking;
  const char* ready;       // printf: ratings, plays, scenes
  const char* found;       // printf: plays
  const char* waitingToo;  // printf: changes
  const char* rules;
  const char* nothing;
  const char* sending;
  const char* done;    // printf: ratings, plays
  const char* failed;  // printf: why
  const char* backup;
  const char* dryRun;
  const char* hideList;
  const char* send;
  const char* close;
  const char* rating;  // printf: stars, Stash's
  const char* none;
  const char* play;
  const char* plays;  // printf: plays
  const char* more;   // printf: items not listed
  const char* tipToggle;
  const char* tipSendAll;
  const char* tipDryRun;
};

// The strip's Scene tab (docs/stash.md): the playing video's Stash details.
struct StashSceneStrings {
  const char* tab;
  const char* yourRating;
  const char* played;      // printf: plays
  const char* lastPlayed;  // printf: how long ago
  const char* notPlayed;
  const char* director;  // printf: name
  const char* ageThen;   // printf: years
  const char* noPerformers;
  const char* onlyMen;
  const char* moreText;
  const char* lessText;
  const char* tags;
  const char* tagOrders[3];  // common first, specific first, A–Z
  const char* pickTags;
  const char* moreTags;  // printf: tags not shown
  const char* fewerTags;
  const char* scenesWithTag;   // printf: the tag
  const char* scenesWithTags;  // printf: how many tags
  const char* moreWith;        // printf: performer
  const char* moreFrom;        // printf: studio
  const char* similar;
  const char* backToDetails;
  const char* similarHint;
  const char* noSimilar;
  const char* with;           // printf: performers
  const char* sameStudio;     // printf: studio
  const char* tagsInCommon;   // printf: how many
  const char* notInStash;
  const char* outsideFolders;  // printf: Stash's folder here
  const char* notScanned;
  const char* notLocal;
  const char* lastSync;  // printf: when, scenes
  const char* nothingPlaying;
  const char* fromStash;  // in place of the path, for a scene played from Stash's stream
  const char* tipTagOrders[3];
  // Edit tags (with Send to Stash on).
  const char* editTags;
  const char* tipEditTags;
  const char* editHint;
  const char* addTags;
  const char* backToTags;
  const char* findTag;     // printf: how many tags
  const char* onScene;
  const char* noTagFound;  // printf: the words
  const char* saveToStash;
  const char* saving;
  const char* noChanges;
  const char* added;    // printf: tags
  const char* removed;  // printf: tags
  const char* discardOne;
  const char* discard;  // printf: changes
  const char* keepEditing;
  const char* nothingSent;
  const char* onlyChanges;
  const char* removesBoth;  // printf: the names
  const char* removesAll;   // printf: how many, the names
  const char* saveFailed;
  const char* tryAgain;
  const char* saved;  // printf: what changed
};

struct FocusStrings {
  const char* allTitle;
  const char* videoTitle;
  const char* stateTitle;
  const char* enabled;
  const char* enabledHint;
  const char* offForAll;
  const char* mode;
  const char* modes[3];
  const char* modeTips[3];
  const char* target;
  const char* targets[2];
  const char* targetHint;
  const char* distance;
  const char* distanceHint;
  const char* gazeDot;
  const char* gazeCorrect;
  const char* gazeCorrectHint;
  const char* baseline;
  const char* baselineHint;
  const char* autoIpd;
  const char* autoIpdHint;
  const char* diagnostics;
  const char* diagnosticsHint;
  const char* motionTitle;
  const char* motion;
  const char* motionDistance;
  const char* motionHint;
  const char* ipd;
  const char* ipdHint;
  const char* makeDefault;
  const char* makeDefaultHint;
  const char* useDefault;
  const char* useDefaultHint;
  const char* ownHint;
  const char* defaultHint;
  const char* unsupported;
  const char* tuningLabels[7];
  const char* tuningHints[7];
};

struct FocusStatusStrings {
  const char* reasons[7];  // FocusReason, except None
  const char* targets[6];  // TargetStatus
  const char* targetTags[6];
  const char* far;
  const char* nothing;
  const char* unavailableBrief;
  const char* unavailable;
  const char* offBrief;
  const char* off;
  const char* manualBrief;  // printf: distance
  const char* manual;       // printf: distance, IPD in mm
  const char* nearest;      // printf: degrees
  const char* measuring;    // printf: distance, optional reason
  const char* measuringBrief;  // printf: optional reason
  const char* holding;      // printf: distance
  const char* heldBrief;    // printf: distance
  const char* following;    // printf: distance, measured distance, confidence, optional reason
};

// A newer release of the managed GitHub install: its dialog and the strip's notice.
struct ReleaseUpdateStrings {
  const char* offer;        // printf: release tag
  const char* downloading;  // printf: release tag
  const char* restarting;   // printf: release tag
  const char* ready;        // printf: release tag; when the app can't restart itself
  const char* downloadFailed;
  const char* updateNow;
  const char* notNow;
  const char* tryAgain;
  const char* close;
  const char* notice;       // also the dialog's title
};

// Settings → Help: saving the logs for a bug report.
struct DiagnosticsStrings {
  const char* group;
  const char* save;
  const char* saving;
  const char* hint;
  const char* saved;   // printf: the file's name
  const char* failed;
  const char* crashed;      // the strip's notice after a crash, leading to Help
  const char* crashedHint;  // Help's line after a crash
};

// The browser's Network page and signing in to SMB servers (menu_network.cpp).
struct NetworkStrings {
  const char* nav;
  const char* looking;
  const char* none;
  const char* lookAgain;
  const char* addServer;
  const char* forget;        // tooltip: a server's trash button
  const char* addTitle;
  const char* signInTitle;   // printf: the server's name
  const char* fields[3];     // server, user name, password
  const char* serverHint;    // placeholders
  const char* userHint;
  const char* show;          // the password
  const char* remember;
  const char* rememberHint;
  const char* signIn;
  const char* signingIn;
  const char* wrongLogin;
  const char* offline;
  const char* denied;
  const char* shareTitle;
  const char* sharesRefused;
  const char* shareField;
  const char* open;
  // SFTP servers (sftp.h): Add server's type and its fields' placeholders ...
  const char* type;
  const char* types[2];      // SMB, SFTP
  const char* typeHint;
  const char* port;
  const char* sftpServerHint;
  const char* sftpUserHint;
  const char* connect;
  const char* connecting;
  const char* offlinePort;   // printf: the server's name, the port
  // ... signing in: with this device's key (and the password once) or a
  // password; what went wrong ...
  const char* signInWith;
  const char* methods[2];    // this device's key, a password
  const char* keyHint;       // printf: the user name
  const char* passwordHint;
  const char* settingUpKey;
  const char* oneTimeCode;   // under wrongLogin
  const char* noSftp;        // printf: the user name, the server's name
  const char* noSftpHint;
  // ... a server taking keys only: add this device's key to the account ...
  const char* keyOnly;       // printf: the server's name, the user name
  const char* deviceKey;
  const char* addKeyLine;    // printf: the server's name, the user name
  const char* tryAgain;
  const char* keyStillRefused;  // printf: the server's name, the user name
  const char* keyStillRefusedHint;
  // ... or, while this device serves it (key_share.h), fetch it with a
  // command ...
  const char* keysOnly;       // printf: the server's name
  const char* runCommand;     // printf: the server's name, the user name
  const char* waitingForKey;  // printf: the server's name
  const char* keyFetched;
  // ... its row in the list (printf: the user name), Forget's tooltip ...
  const char* byline;
  const char* forgetKeyTip;
  // ... its key changed ...
  const char* changedTitle;  // printf: the server's name
  const char* changedText;
  const char* trustedKey;
  const char* nowKey;
  const char* dontConnect;
  const char* trustNewKey;
  // ... signed in with the password, the key refused ...
  const char* keyRefusedTitle;
  const char* keyRefusedText;  // printf: the server's name
  const char* keyRefusedHint;
  const char* notNow;
  // ... and Forget, its key taken off the account too.
  const char* forgetTitle;     // printf: the server's name
  const char* forgetKeyText;   // printf: the user name
  const char* forgetOnly;
  const char* removeKeyForget;
  const char* removingKey;
  const char* forgetOffline;   // printf: the server's name
  const char* forgetAnyway;
};

// Where Stash's videos are here (menu_stash_folders.cpp): found, or chosen.
struct StashFoldersStrings {
  const char* choose;       // beside Stash's folders field
  const char* tipChoose;
  const char* looking;
  const char* foundIn;      // printf: the folder here; Stash's videos found nowhere else
  const char* alsoIn;       // printf: ... found where they are now too
  const char* use;
  const char* tipUse;       // printf: the shown folder on this device
  const char* notFound;
  const char* pickTitle;    // the browser's heading while choosing, before the folder
  const char* notInFolder;  // ... once one without them was chosen
  const char* useFolder;
  const char* checking;
  // Several of Stash's folders: under each, where it is here or that it
  // wasn't found; and the search's progress.
  const char* folderHere;      // printf: the folder here
  const char* folderNotHere;
  const char* folderRest;      // under that, when some of its scenes play from Stash's stream
  const char* folderStreamed;  // not here, its scenes play from Stash's stream
  const char* streamed;        // one folder: those not here play from Stash's stream
  const char* lookingFolders;  // printf: the folder looked for, how many
  const char* otherFiles;      // the row of Stash's files in none of its library folders
};

// The on-screen keyboard (menu_search.cpp); its other keys are in Strings.
struct KeyboardStrings {
  const char* capsLock;  // Shift tapped twice
  const char* tipShift;
  const char* tipLayout;   // printf: the layout's native name a tap switches to
  const char* tipLayouts;  // no other layout yet: a tap lists them
  const char* tipLeft;     // ← and →: the text cursor
  const char* tipRight;
};

// The surround viewpoint moved toward or away from the picture (the stick's
// forward and back).
struct ViewpointStrings {
  const char* label;    // Video settings' row (only 180° and 360° video have one)
  const char* closer;   // printf format with one %d (percent of the sphere's radius)
  const char* back;     // the same, away from the picture
  const char* centered;
  const char* overlay;  // printf format with one %s (closer, back or centered), shown as it changes
  const char* tip;
};

// Passthrough switching itself on (Settings → View, see_through.h).
struct AutoPassthroughStrings {
  const char* setting;
  const char* hint;         // Frame
  const char* hintDesktop;  // a PC: only for headsets whose camera keys the green
  const char* mask;         // shown in front of the eyes as it switches on
  const char* greenScreen;
  const char* undo;         // the line below either
};

// microSD cards and USB drives (removable_drives.h): the navigation's name
// for one without a label, and the notice when one is put in.
struct DriveStrings {
  const char* card;
  const char* usbDrive;
  const char* drive;
  const char* inExplorer;  // below its name, once mounted
  const char* cardFailed;  // above why it could not be opened
  const char* usbFailed;
};

// Settings → Appearance: the menu's colour (accent.h).
struct AccentStrings {
  const char* group;
  const char* colour;
  const char* names[8];  // the presets and Custom, as their swatches' tooltips
  const char* hue;
  const char* brightness;
  const char* brightnessHint;
  const char* saturation;
};

// Offline downloads (offline.h, menu_offline.cpp): the Offline place, the
// marks on rows and tiles, the questions before a download, the setting.
// printf formats where marked.
struct OfflineStrings {
  const char* nav;
  const char* toGo;           // the place's sections
  const char* onDevice;
  const char* summary;        // printf: downloads to go, bytes left
  const char* summaryTime;    // printf: ... and the time left
  const char* summaryDone;    // printf: videos on this device, their size
  const char* empty;
  const char* pauseAll;
  const char* resumeAll;
  const char* pause;
  const char* resume;
  const char* tryAgain;
  const char* newDownloads;   // the drive new downloads go to
  const char* offlineVideos;  // printf: their size on a drive
  const char* freeOf;         // printf: free, total
  const char* internal;       // the drive in the headset or computer
  const char* internalHeadset;
  const char* internalComputer;
  const char* queued;
  const char* queuedSize;     // printf: size
  const char* copying;        // printf: percent, copied, size, speed, time left, drive
  const char* copyingStart;   // printf: percent, copied, size, drive (speed not known yet)
  const char* paused;         // printf: percent, copied, size
  const char* pausedPlaying;  // printf: percent, copied, size
  const char* waitingServer;
  const char* waitingDrive;
  const char* restarting;     // the copy didn't match its source
  const char* failedGone;
  const char* failedNoRoom;
  const char* missing;        // done, but the copy isn't there now
  const char* shortStates[3];  // on thumbnails: queued, waiting, failed
  const char* minutes;        // printf: minutes left
  const char* hours;          // printf: hours, minutes
  const char* underMinute;
  const char* tipDownload;
  const char* tipDownloaded;  // printf: the drive
  const char* tipShow;        // a thumbnail's state: opens the place
  const char* tipCancel;
  const char* tipRemove;
  const char* saveToTitle;
  const char* saveToHint;
  const char* noRoomTitle;    // printf: the drive
  const char* needs;          // printf: needed, free
  const char* noRoomHint;     // printf: the other drive
  const char* noRoomHintOnly;
  const char* instead;        // printf: the other drive, its free space
  const char* removeTitle;
  const char* removeWhat;     // printf: name, size, drive
  const char* removeWarning;  // printf: the server
  const char* removeWarningLocal;
  const char* remove;
  const char* download;       // the Scene tab's box
  const char* downloaded;     // printf: the drive
  const char* setting;
  const char* settingHint;
  const char* serverDown;     // tooltip of a server's place while it doesn't answer
};

// The strip's red label while a video plays in software decoding, and its tooltip.
struct SoftwareDecodingStrings {
  const char* label;
  const char* noHardware;  // printf: the codec; the Frame decodes only H.264 and HEVC
  const char* unavailable;
};

// Edit places: the navigation's places in the user's order, shown or not,
// renamed.
struct PlacesStrings {
  const char* edit;  // the navigation's link to the page
  const char* title;
  const char* hint;
  const char* name;  // column headings
  const char* location;
  const char* show;
  const char* tipRename;
  const char* tipMoveUp;
  const char* tipMoveDown;
  const char* reset;
  const char* tipReset;
  const char* renameTitle;  // printf: its own name
  const char* renameHint;
  const char* save;
};

// Presets atop Video settings: the row, the list of the user's own and
// naming one. printf formats where marked.
struct PresetStrings {
  const char* label;     // the row's
  const char* names[3];  // Low, Balanced, High
  const char* custom;    // ... none of them: changed by hand
  const char* tipCustom;
  const char* myPresets;  // the list's title and its button's tooltip
  const char* none;
  const char* saveAs;
  const char* tipMoveUp;
  const char* tipMoveDown;
  const char* tipRename;
  const char* tipDelete;
  const char* deleteAsk;  // printf: the name
  const char* deleteButton;
  const char* saveTitle;
  const char* renameTitle;  // printf: the name
  const char* name;         // the field's label
  const char* save;
  const char* replace;
  const char* rename;
  const char* savesThis;    // printf: the settings, listed
  const char* itsSettings;  // printf: the settings, listed
  const char* saveHint;
  const char* renameHint;
  const char* builtInName;  // printf: the name typed
  const char* nameTaken;    // printf: the preset's name
  const char* replaceWarning;  // printf: the preset's name
  const char* item;       // a setting in a preset's summary; printf: its name, its value
  const char* separator;  // between them
};

// Setting up Stash: signing in from the server's editor instead of typing
// the API key, and what a sync is doing while there's no copy yet. The
// user name and password fields, Show and Signing in are NetworkStrings'.
struct StashSetupStrings {
  const char* signInHint;
  const char* wrongLogin;
  const char* connecting;   // a sync's steps
  const char* downloading;  // printf: megabytes received so far, e.g. "3.4"
  const char* downloadingStart;  // ... under 0.1
  const char* saving;
  const char* building;
  const char* firstSync;    // under them while there's no copy of Stash yet
  // The server field's placeholder, and why Stash can't be used.
  const char* serverHint;
  const char* tooOld;
  const char* needsSignIn;
  const char* keyRefused;
  const char* publicRefused;  // Stash's 403 for an address outside its network without a login
  const char* forbidden;
  const char* refused;        // nothing answers at the address
  const char* noHost;
  const char* timeout;
  const char* certificate;
  const char* noSecure;       // https to a server that doesn't speak it
  const char* badAddress;
  const char* unreachable;
  const char* tryHttps;       // http to a server that only speaks https
  const char* notStash;
  const char* httpError;      // printf: the HTTP status
  const char* elsewhere;      // a redirect to another server
  const char* stashSaid;      // printf: Stash's own error message
};

// First-run setup (docs/alpha/first-run.md, menu_setup.cpp).
struct SetupStrings {
  // The desktop window, until someone wears the headset.
  const char* putOnHeadset;
  const char* startsWhenWorn;
  const char* setUpLater;
  const char* back;
  const char* next;  // Continue
  // Welcome.
  const char* welcome;
  const char* getStarted;
  const char* pointAndPull;  // after the trigger
  const char* turnOnController;
  const char* pressSteamButton;  // Steam Frame
  const char* holdPowerButton;   // another headset's controllers
  // The steps, their titles and lines.
  const char* controlsTitle;
  const char* controlsText;
  const char* turnOnControllers;  // Your controllers with none on (desktop)
  const char* stashTitle;
  const char* stashText;
  // Do you use Stash?: what it adds, six titles each with its line (the
  // last line elsewhere than on the Steam Frame, and with no folders
  // added); then the line under them (... with no folders added), and its
  // buttons.
  const char* stashFeatures[12];
  const char* keptOnComputer;
  const char* forDownloads;
  const char* stashFree;
  const char* stashFreeNoFolders;
  const char* notNow;
  const char* connectStash;  // ... and the address page's title
  const char* stashConnected;  // printf: the address; Stash already set up
  const char* change;
  // The address, Connect, and why Stash can't be used.
  const char* addressText;
  const char* address;
  const char* connect;
  const char* connecting;
  const char* unreachableHeadset;   // the Steam Frame
  const char* unreachableComputer;  // ... elsewhere
  // Sign in to Stash.
  const char* stashSignInTitle;
  const char* stashSignInText;
  const char* videosTitle;
  const char* videosText;
  // The menu's card, after Not now.
  const char* useStash;
  const char* dontAsk;
  // Setup out of view: the arrow's label, and the line after 2 s.
  const char* cue;
  const char* bringHere;  // printf: the menu button's name
  const char* buttonB;    // ... none bound
  // Add folders: the places (the home folder's, on the Steam Frame and
  // elsewhere; the network's are under NetworkStrings::nav), ...
  const char* thisHeadset;
  const char* thisComputer;
  const char* addComputer;
  const char* looking;  // ... while computers are looked for
  // ... the home folder's tree: its two tops; a card's or drive's first row ...
  const char* home;
  const char* fileSystem;
  const char* wholeCard;
  const char* wholeDrive;
  // ... a folder opened: while it is read, then the videos directly in it
  // (with subfolders and without), or that it can't be read ...
  const char* opening;
  const char* oneVideoToo;
  const char* videosToo;  // printf: how many
  const char* oneVideo;
  const char* videos;     // printf: how many
  const char* empty;
  const char* cantOpen;
  // ... a computer added by its name or address ...
  const char* addComputerTitle;
  const char* computerField;
  // ... and the footer's button.
  const char* addOneFolder;
  const char* addFolders;  // printf: how many
  const char* skip;
  // After Add: what was added.
  const char* oneFolderAdded;
  const char* foldersAdded;  // printf: how many
  const char* appearAsFound;
  const char* addMore;
  // The Library once setup has ended: the banner (the quality preset in
  // effect, and where to change it) ...
  const char* allSetFrame;    // High on the Steam Frame
  const char* allSetDesktop;  // Balanced elsewhere
  const char* allSet;         // printf: another preset's name
  const char* changeItIn;     // before the link to Video settings
  const char* dismiss;        // its ×
  // ... and Stash's first sync, while it runs or after it failed: nothing
  // pinned, the page says so; with pins, a line above them.
  const char* gettingScenes;
  const char* gettingScenesText;
  const char* gettingScenesLine;  // printf: the sync's step
  const char* couldntGetScenes;
  const char* couldntGetLine;     // printf: why
  const char* stoppedAnswering;       // why, when Stash stopped answering: the page's
  const char* stoppedAnsweringShort;  // ... and the line's
  const char* tryAgain;
  const char* stashSettings;
  // An SFTP computer (with NetworkStrings' SFTP lines): Add a computer's
  // type line and user name placeholder; then [0] on the Steam Frame, [1]
  // elsewhere: the sign-in's key choice, its line and the password's ...
  const char* typeHint;
  const char* userHint;
  const char* methodKey[2];
  const char* keyChoiceHint[2];
  const char* passwordChoiceHint[2];
  const char* keyOnce[2];          // printf: the user name
  // ... a computer taking keys only (printf: its name, the user name) ...
  const char* keyOnly[2];
  const char* ownKey[2];
  const char* keyStillRefused[2];  // printf: its name, the user name
  // ... and signed in with the password, the key refused (printf: its name).
  const char* keyRefusedText[2];
};

// What the controls do and their names (controller_guide.h): setup's Your
// controllers page and the hints.
struct ControllerStrings {
  // A label's title, and its line (printf: the controls' names); Menu's,
  // Play or pause's and Recenter's line is the names alone.
  const char* choose;
  const char* chooseLine;
  const char* move;
  const char* moveLine;
  const char* menu;
  const char* playPause;
  const char* recenter;
  const char* turn;  // the left stick, sideways
  const char* seek;  // the right one
  const char* sideways;
  const char* scroll;
  const char* upDown;
  // Without a picture: a column for each controller, and for what either does.
  const char* left;
  const char* right;
  const char* either;
  // The controls' names (A, B, X and Y are their letters), as in a sentence:
  // "grip", but "Menu" (a line starting with one gets its capital).
  const char* trigger;
  const char* bumper;
  const char* grip;
  const char* stick;
  const char* stickClick;
  const char* trackpad;
  const char* menuButton;
  const char* viewButton;
  const char* selectButton;
  const char* dpad;  // printf: the directions
  const char* directions[4];  // up, down, left, right
  // Lists of names: "A, B or C".
  const char* comma;
  const char* orLast;
  // The hints after setup, one line each (printf: the button's name).
  const char* menuHint;           // the menu closed
  const char* scrollHint;         // a long list pointed at
  const char* recenterRightHint;  // the video far off to one side: its control is the right stick
  const char* recenterLeftHint;   // ... the left stick
  const char* recenterHint;       // ... a button
  const char* volumeHint;         // pointing at the volume (printf: grip's name, then the trigger's)
};

// Deleting the playing file (docs/alpha/delete-file.md): the delete
// dialog, and why a deletion failed.
struct DeleteStrings {
  const char* onDevice;  // where the file is
  const char* onServer;  // printf: the SMB server's name
  const char* forGood;
  const char* deleteButton;
  const char* removeDownload;  // a downloaded copy: only it
  const char* everywhere;      // ... it and its source
  const char* sourceChanged;   // ... refused: the source isn't the video downloaded
  const char* deleting;
  const char* changed;
  const char* gone;
  const char* notAFile;
  const char* denied;
  const char* inUse;
  const char* offline;
  // A Stash scene's file (Send to Stash on).
  const char* inStash;           // where a scene played from Stash's stream is
  const char* stashTrash;        // in place of forGood: Stash has a trash folder
  const char* stashAlso;         // the scene goes too
  const char* stashKeepsOther;   // the scene has one other file
  const char* stashKeepsOthers;  // printf: how many other files
  const char* stashLists;        // Stash couldn't delete it; deleted here
  const char* stashUnsure;       // ... as Stash's files didn't show which it is
  const char* stashKeptHere;     // Stash deleted its file, the one here failed its check
  const char* deletedTitle;      // the dialog after, with stashLists
  const char* tipNeedsSendToStash;
  const char* tipStashDown;
};

// All text shown in VR.
struct Strings {
  const char* noVideo;
  const char* loading;
  const char* playbackFailed;
  const char* format;
  const char* projections[7];  // by Projection value
  const char* stereo[3];
  const char* curved;
  const char* passthrough;
  const char* passthroughHint;
  const char* recenter;
  const char* settings;
  const char* files;
  const char* back;
  const char* noVideos;
  const char* noSubtitleFiles;
  const char* chooseSubtitleFile;
  const char* noAudio;
  const char* subtitles;
  const char* off;
  const char* on;
  const char* track;
  const char* subtitles3d;
  const char* subtitles3dHint;
  const char* subtitleFromFile;
  const char* space;
  const char* paste;
  const char* deleteTitle;
  const char* deleteFailed;
  const char* cancel;
  const char* ok;
  const char* quitTitle;
  const char* quit;
  const char* picture;
  const char* pictureHint;
  const char* brightness;
  const char* contrast;
  const char* saturation;
  const char* zoom;
  const char* reset;
  const char* design;
  const char* themes[2];            // glass, material
  const char* projectionShort[7];   // format tiles
  const char* stereoShort[3];
  // Tooltips of icon buttons.
  const char* tipDelete;
  const char* tipPrevious;
  const char* tipNext;
  const char* tipBack10;
  const char* tipForward10;
  const char* tipPlay;
  const char* tipPause;
  const char* tipStop;
  const char* tipMute;
  const char* tipSlideshow;
  const char* deletePhotoTitle;
  const char* tipDeletePhoto;
  const char* sortModes[14];  // by FolderListing::Order
  const char* sortFields[7];  // name, date, size, rating, plays, last played, date added
  const char* tipTop;
  const char* tipBottom;
  const char* tipGallery;
  const char* tipList;
  const char* pinFolder;
  const char* unpinFolder;
  const char* folderUnavailable;
  const char* previousPlaces;
  const char* morePlaces;
  const char* background;
  const char* recenterIn;  // printf format with one %d (seconds)
  const char* subtitleSize;
  const char* subtitleRaise;
  const char* subtitleDistance;
  const char* chapter;
  const char* tipSearchFolder;
  const char* clear;
  const char* noMatches;  // printf format: the search words
  const char* matches;    // printf format: matches, total
  const char* greenScreen;
  const char* greenScreenHint;
  const char* greenScreenOwnMask;
  const char* passthroughGroup;  // video settings: the group with the mask and the green screen
  const char* opacity;
  const char* opacityHint;
  const char* opacityOverlay;  // printf format with one %d (percent), shown as a twist changes it
  const char* packedMask;
  const char* packedMaskHint;
  const char* packedMaskFormatHint;
  const char* keyStrength;
  const char* keySoftness;
  const char* keySpill;
  const char* keyShrink;
  const char* greenScreenResetHint;
  const char* resetSliders;
  const char* halfSbs;  // format tile: flat 3D with half-squeezed eyes
  const char* halfTb;
  const char* halfPackedHint;
  const char* allVideos;     // picture page: heading of the settings for every video
  const char* videoMipmaps;  // on = trilinear through mipmaps, off = full-size level only
  const char* antiShimmer;   // choice: how much fine lines are smoothed with mipmaps off
  const char* antiShimmerLevels[3];  // off, on, strong
  const char* antiShimmerHint;       // what it does; Strong's cost
  const char* antiShimmerFrameHint;  // ... on the Frame, where Strong costs more
  const char* antiShimmerMipmaps;    // not used while Video mipmaps is on
  const char* sharpen;       // followed by the strength
  const char* sharpenOff;
  const char* toneLook;      // toggle: tone curve and vignette
  const char* toneLookHint;
  const char* tvGamma;       // global toggle: BT.1886 conversion
  const char* tvGammaHint;
  const char* eyeResolution;       // eye image size (video and menus), followed by the factor and pixels
  const char* resolutionHint;      // Frame, printf format: the panel width, the instant cap
  const char* resolutionSteamVr;   // ... 1× is SteamVR's recommended size
  const char* resolutionOverride;  // printf format: this run's eye image pixels are the command line's
  const char* resolutionLimited;   // printf format: the scale SteamVR or the GPU allowed
  const char* resolutionChanged;   // printf format: 1× is now the panel's pixels; the nearest size
  const char* resolutionPending;   // while its slider is held: the size switches on letting go
  const char* refreshRate;         // choice: the headset's refresh rate (Frame)
  const char* refreshRateChoice;   // printf format: a rate in Hz
  const char* foveation;           // choice: how much less detail away from where you look
  const char* foveationLevels[5];  // off, low, medium, high, custom
  const char* foveationHint;
  const char* showFoveation;
  const char* showFoveationHint;
  const char* foveationFrameOnly;
  const char* sharpArea;
  const char* sharpBins[2];
  const char* sharpSizeHint;
  const char* foveationRing;
  const char* foveationOutside;
  const char* foveationRates[2];
  const char* foveationSame;
  const char* advancedSettings;
  const char* sharpBinsUsage;
  const char* foveationRingUsage;
  const char* refreshRateHelp;
  const char* refreshRateUsage;
  const char* refreshRateUnavailable;
  const char* refreshRateOffered;
  const char* refreshRateCurrent;
  const char* refreshRateRequested;
  const char* refreshRateChanged;
  const char* unknownOption;
  const char* tipParent;
  const char* done;
  const char* readingFolder;
  const char* groupMenu;  // settings group titles
  const char* groupView;
  const char* stereoFocus;
  const char* groupScreen;  // the flat screen's distance, height and curve
  const char* imageAllVideos;
  const char* greenScreenOn;
  const char* videoSettings;
  const char* tabMainMenu;
  const char* quitApp;
  const char* fileExplorer;
  const char* placeHome;
  const char* placeVideos;
  const char* placeFileSystem;
  const char* sortBy;
  const char* backspace;
  const char* shift;
  const char* symbolKeys;  // the keyboard's key to its symbols
  const char* letterKeys;  // ... and back to the letters
  const char* hide;
  const char* groupPlayback;
  const char* resume;  // the setting
  const char* resumeHint;
  const char* seekPreview;  // the setting
  const char* seekPreviewHint;
  const char* seekPreviewUnavailable;  // why it is off on the Steam Frame
  const char* showPerformance;  // the setting: the performance readout (performance_readout.h)
  const char* showPerformanceHint;
  const char* vibration;  // the setting: controllers buzz on clicks, seeks and grabs
  const char* vibrationHint;
  // The readout's lines; printf formats where marked.
  const char* perfHeadset;        // printf: frames drawn per second, the headset's rate
  const char* perfVideo;          // printf: new video frames per second, the file's rate
  const char* perfVideoUnknown;   // printf: ... the file's rate not known
  const char* perfNoVideo;
  const char* perfPaused;
  const char* perfWaiting;        // loading or seeking
  const char* perfDropped;        // printf: drops in the last seconds, seconds, drops since, perfSince*
  const char* perfSinceOpen;
  const char* perfSinceSeek;
  const char* perfSize;           // printf: eye image pixels across
  const char* perfGpu;            // printf: GPU milliseconds per headset frame
  const char* perfSharp;          // printf: the sharp area, degrees across and down
  const char* perfSoftware;       // mpv's decoder is software
  const char* perfDirect;         // the eyes sample decoded frames (docs/vulkan-renderer.md 11)
  const char* perfMpv;            // ... mpv draws them
  const char* thumbnails;  // the setting
  const char* thumbnailsHint;
  const char* library;  // the library view
  const char* tipSearchLibrary;
  const char* libraryEmpty;
  const char* libraryEmptyStash;  // ... with Stash on, before its first catalog
  const char* libraryNoMatches;  // printf format: the search words
  const char* libraryCount;      // printf format: videos
  const char* libraryCountOne;   // ... one
  const char* libraryMatches;    // printf format: matches, videos
  const char* libraryCountSoFar;  // printf format: videos (a pin's first look still runs)
  const char* libraryShows[4];    // All, Not played, Played, Rated
  const char* libraryUpToDate;    // printf format: how long ago the last look was
  const char* libraryLooking;     // printf format: percent
  const char* libraryLookingUnknown;
  const char* libraryLookWaits;   // a look waits for no video to be open
  const char* tipLookAgain;
  const char* tipLookingNow;
  const char* tipLookWaits;
  const char* libraryNoPinsTitle;
  const char* libraryNoPinsText;
  const char* libraryNoPinsStash;  // ... with Stash on, before its first catalog
  const char* openFileExplorer;
  const char* screenDistance;
  const char* screenHeight;
  const char* screenCurve;
  const char* screenHint;  // how the controllers change them too
  const char* screenResetHint;
  StashStrings stash;
  StashSceneStrings stashScene;
  StashWriteBackStrings stashWriteBack;
  FocusStrings focus;
  const char* tipAdvanced;
  const char* tipSubtitleRaise;
  const char* tipPictureReset;
  const char* tipMipmaps;
  const char* tipMipmapsFrame;
  const char* tipSharpen;
  const char* tipSharpenFrame;
  const char* tipAntiShimmerFrame;
  const char* tipResolution;
  const char* tipResolutionFrame;
  const char* tipRefreshRate;
  const char* tipFoveationOffFrame;
  const char* tipSharpArea;
  const char* tipFoveationRing;
  const char* tipFoveationOutside;
  const char* tipKey[4];  // strength, shrink, edges, spill
  const char* tipScreenHeight;
  const char* tipScreenCurve;
  const char* tipRating;
  const char* tipVolume;
  const char* tipSortColumn;
  const char* tipHideKeyboard;
  const char* tipHideServerKeyboard;
  const char* language;
  const char* automaticLanguage;
  const char* languageName;  // native name, shown in every language's chooser
  const char* languageHint;
  FocusStatusStrings focusStatus;
  ReleaseUpdateStrings releaseUpdate;
  DiagnosticsStrings diagnostics;
  NetworkStrings network;
  StashFoldersStrings stashFolders;
  KeyboardStrings keyboard;
  ViewpointStrings viewpoint;
  AutoPassthroughStrings autoPassthrough;
  DriveStrings drives;
  OfflineStrings offline;
  SoftwareDecodingStrings softwareDecoding;
  PlacesStrings places;
  AccentStrings accent;
  PresetStrings presets;
  StashSetupStrings stashSetup;
  SetupStrings setup;
  ControllerStrings controls;
  DeleteStrings deletion;
};

const Strings& strings();
// Choice 0 is Automatic. Persist codes, not positions, in settings.conf.
int languageCount();
const char* languageCode(int choice);
std::string languageName(int choice);
int languageChoice();
void setLanguage(const std::string& code);
// The code of the language shown, Automatic resolved ("de", "pt_BR").
const char* shownLanguageCode();

// A time as a clock shows it: "4:05", or "1:04:05" from an hour on.
std::string clockTime(double seconds);
// The viewpoint's place, `percent` of the sphere's radius toward the picture:
// "15 % closer", "10 % back" or "Centered".
std::string viewpointText(int percent);
