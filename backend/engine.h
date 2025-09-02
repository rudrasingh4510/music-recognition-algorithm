#pragma once
#include <string>
#include <vector>

struct Song {
    int id;
    std::string name;
    size_t numFingerprints;
    std::string youtube_url; // <-- ADD THIS LINE
};

void engine_init(const std::string& data_dir);
// Update function signature to accept the URL
int add_song_to_db(const std::string& path, const std::string& displayName, const std::string& youtube_url = "");
std::string identify_from_file(const std::string& path);
std::vector<Song> get_song_list();