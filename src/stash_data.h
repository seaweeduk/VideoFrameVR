#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Everything a sync takes from a Stash server, as the server has it: what
// StashClient fetches, StashStore keeps in stash.db and StashCatalog is
// built from. Ids are Stash's own (0 = none); text Stash leaves out is
// empty; image URLs are empty where Stash has no image.
struct StashData {
  struct File {
    // The server's id of it, where its stream is asked for by one
    // (MediaServer::streamPath()); 0 where a scene's is its first file's.
    int64_t id = 0;
    std::string path;      // Stash's path (stash_settings.h maps it)
    uint64_t size = 0;
    int64_t modified = 0;  // Unix seconds
    int width = 0, height = 0;
    double duration = 0;   // seconds
    double fps = 0;
    std::string codec;
    std::string oshash;
  };
  struct Scene {
    int64_t id = 0;
    std::string title, code, details, director;
    std::string date;       // release date, "YYYY-MM-DD"
    int64_t createdAt = 0;  // added to Stash, Unix seconds
    int64_t studio = 0;
    std::vector<int64_t> performers, tags;
    std::vector<std::pair<int64_t, int>> groups;  // group, scene number in it (0 = none)
    std::vector<std::string> urls;
    std::string cover;
    bool vr = false;  // the server says it is VR, where it says (else its VR tag does)
    std::vector<File> files;  // the primary file first
  };
  struct Performer {
    int64_t id = 0;
    std::string name, disambiguation;
    std::vector<std::string> aliases;
    std::string gender;  // Stash's GenderEnum: FEMALE, MALE, TRANSGENDER_FEMALE, ...
    std::string birthdate, country, ethnicity, hairColor, eyeColor;
    int heightCm = 0;
    std::string measurements, fakeTits, tattoos, piercings, careerStart, careerEnd;
    std::string image;
    bool favorite = false;
  };
  struct Studio {
    int64_t id = 0;
    std::string name, image;
    int64_t parent = 0;  // the studio it is under
  };
  struct Tag {
    int64_t id = 0;
    std::string name;
  };
  struct Group {
    int64_t id = 0;
    std::string name, date, image;
    int64_t studio = 0;
  };

  std::string kind = "stash";  // MediaServer::id() of the server synced from
  std::string server;   // the URL synced from
  std::string version;  // Stash's, e.g. "v0.31.1"
  std::string vrTag;    // the name of the tag Stash marks VR scenes with
  // Stash's library folders with videos, as it lists them (its paths); none
  // from a copy made before they were kept.
  std::vector<std::string> libraryFolders;
  int64_t syncedAt = 0;
  std::vector<Scene> scenes;
  std::vector<Performer> performers;
  std::vector<Studio> studios;
  std::vector<Tag> tags;
  std::vector<Group> groups;
};
