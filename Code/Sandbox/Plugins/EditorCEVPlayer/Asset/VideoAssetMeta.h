// What the editor stores in the "Details" of a video asset's .cryasset (editor only, not shipped) and helpers to
// read / write it.
//
// Files of one video asset (all flat in one folder, every name starts with the asset name, see docs/research/04 §2.3):
//   <name>.cevvideo               runtime descriptor (Files[0])
//   <name>.webm                   the video: copy of a WebM source, or what FFmpeg made of the source (not shipped when the descriptor points elsewhere)
//   <name>_source.<ext>           source copy of an FFmpeg import / re-encode (never shipped; no copy for a PNG sequence)
//   <name>_<height>p.webm         variants listed in the descriptor <Variants>
//   <name>_silent.webm            picture only copy (after "strip audio")
//   <name>_<track>.webm|.wav|.ogg   external audio tracks (split / exported), named after the descriptor's audio track
// The SDL_mixer copies live in audio/sdlmixer/assets/cev/<name>/ and the ACE library in audio/sdlmixer/ace/cev_<name>.xml
// (Details sdlFiles / aceLibrary), they are not Files of the asset.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include <CryString/CryString.h>

class CAsset;

namespace CEVEditor
{
	namespace DetailKey
	{
		constexpr const char* kSourcePath     = "sourcePath";         // relative to Assets: the copy of the source video next to the asset
		constexpr const char* kSourceOriginal = "sourceOriginalPath"; // absolute path of the file that was imported
		constexpr const char* kSourceSha256   = "sourceSha256";       // hash of the copy at import time
		constexpr const char* kSourceSize     = "sourceSize";
		constexpr const char* kSourceStamp   = "sourceStamp";        // "<size>:<mtime>" of the source copy when its hash was last taken
		constexpr const char* kOriginalStamp = "originalStamp";      // "<size>:<mtime>" of the original file when its hash was last taken
		constexpr const char* kImportSettings = "importSettings";     // key=value;key=value
		constexpr const char* kImportState    = "importState";        // ready | processing | failed
		constexpr const char* kWidth          = "width";
		constexpr const char* kHeight         = "height";
		constexpr const char* kDuration       = "duration";           // seconds
		constexpr const char* kFps            = "fps";
		constexpr const char* kCodec          = "codec";
		constexpr const char* kAlpha          = "alpha";              // 0 / 1
		constexpr const char* kAudioTracks    = "audioTracks";
		constexpr const char* kRoute          = "route";
		constexpr const char* kTrackNames     = "audioTrackNames";    // comma separated names of the descriptor's audio tracks
		constexpr const char* kSdlFiles       = "sdlFiles";           // ';' separated paths relative to Assets
		constexpr const char* kAceLibrary     = "aceLibrary";         // path relative to Assets
		constexpr const char* kTranscoded     = "transcoded";         // 1: <name>.webm was made by FFmpeg from the source copy / PNG sequence
		constexpr const char* kSourceKind     = "sourceKind";         // webm | video | png-sequence
	}

	//! Name -> value pairs of a Details block with typed access.
	class CDetails
	{
	public:
		CDetails() = default;
		explicit CDetails(const CAsset& asset);

		void Set(const char* key, const std::string& value);
		void SetInt(const char* key, int value);
		void SetDouble(const char* key, double value);
		std::string Get(const char* key) const;
		int GetInt(const char* key, int def = 0) const;
		double GetDouble(const char* key, double def = 0.0) const;
		bool Has(const char* key) const;
		//! The pairs in insertion order (what IEditableAsset::SetDetails takes).
		std::vector<std::pair<string, string>> ToPairs() const;

	private:
		std::vector<std::pair<std::string, std::string>> m_pairs;
	};

	//! "a=1;b=2" <-> map helpers for the importSettings detail.
	std::string JoinSettings(const std::vector<std::pair<std::string, std::string>>& kv);
	std::vector<std::pair<std::string, std::string>> SplitSettings(const std::string& text);
	std::vector<std::string> SplitList(const std::string& text, char sep);
	std::string JoinList(const std::vector<std::string>& items, char sep);

	//! Absolute path (UTF-8, forward slashes) of a path relative to the game's Assets folder.
	std::string AssetsAbsolute(const std::string& relPath);
	//! Folder part of a relative path ("a/b/c.x" -> "a/b/", "c.x" -> "").
	std::string FolderOf(const std::string& path);
	std::string FileNameOf(const std::string& path);
	std::string StemOf(const std::string& path);
}
