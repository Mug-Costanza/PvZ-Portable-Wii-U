// Pre-renders PvZ's tracker music (sounds/mainmusic.mo3) into one Ogg Vorbis
// file per tune, for the Wii build: libopenmpt would need ~31 MB of RAM to
// play the module directly (it decodes all 198 samples up front), far more
// than the Wii has left. The Wii build streams these files from the SD card
// instead (see Music.cpp, PVZ_PRERENDERED_MUSIC).
//
// Each tune is rendered from its start order (the offsets in
// Music::PlayMusic) until the module loops back, with the channels the game
// keeps silent when a tune starts (drums/hi-hats, per SetupVolumeForTune)
// muted - i.e. what plays before any "burst" layers fade in.
//
// Several tunes have an intro and loop back to a later order (Grasswalk
// jumps back to 0x0C, for example), so each file gets LOOPSTART/LOOPEND
// Vorbis comments (in samples), which SDL_mixer honours for seamless
// looping of just the looping part.
//
// Usage: render_music <main.pak> <output dir>
// Reads the game's own data; nothing copyrighted is stored in the repo.

#include <libopenmpt/libopenmpt.hpp>
#include <libopenmpt/libopenmpt_ext.hpp>
#include <vorbis/vorbisenc.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{

constexpr int kSampleRate = 22050; // the Wii build's mixer rate (SDLSoundManager)
constexpr double kMaxSeconds = 600;

struct Tune
{
	const char* mFile;	// sounds/prerendered/<mFile>.ogg - keep in sync with Music.cpp
	int mStartOrder;	// Music::PlayMusic
	int mMainEnd;		// last channel that plays at the start (SetupVolumeForTune)
};

const Tune kTunes[] = {
	{ "grasswalk",       0x00, 23 },
	{ "moongrains",      0x30, 29 },
	{ "waterygraves",    0x5E, 17 },
	{ "rigormormist",    0x7D, 15 },
	{ "grazetheroof",    0xB8, 17 },
	{ "chooseyourseeds", 0x7A, 29 },
	{ "crazydave",       0x98, 29 },
	{ "zengarden",       0xDD, 29 },
	{ "cerebrawl",       0xB1, 29 },
	{ "loonboon",        0xA6, 29 },
	{ "conveyer",        0xD4, 29 },
	{ "brainiacmaniac",  0x9E, 29 },
};

// main.pak: every byte XORed with 0xF7; a header of records (flags, name,
// size, filetime) terminated by flag 0x80, followed by the files' data.
std::vector<char> ReadFromPak(const char* thePakPath, const std::string& theWanted)
{
	std::ifstream aFile(thePakPath, std::ios::binary);
	std::vector<char> aPak((std::istreambuf_iterator<char>(aFile)), {});
	for (char& c : aPak)
		c ^= (char)0xF7;
	auto u32 = [&](size_t p) { uint32_t v; memcpy(&v, &aPak[p], 4); return v; }; // little-endian host

	if (aPak.size() < 8 || u32(0) != 0xBAC04AC0)
		return {};
	size_t aPos = 8;
	struct Rec { std::string mName; uint32_t mSize; };
	std::vector<Rec> aRecs;
	while (aPos < aPak.size() && !(aPak[aPos] & 0x80))
	{
		uint8_t aLen = (uint8_t)aPak[aPos + 1];
		std::string aName(&aPak[aPos + 2], aLen);
		for (char& c : aName)
			c = (c == '\\') ? '/' : (char)tolower((unsigned char)c);
		aRecs.push_back({ aName, u32(aPos + 2 + aLen) });
		aPos += 2 + aLen + 4 + 8;
	}
	size_t aData = aPos + 1;
	for (const Rec& r : aRecs)
	{
		if (r.mName == theWanted)
			return std::vector<char>(aPak.begin() + aData, aPak.begin() + aData + r.mSize);
		aData += r.mSize;
	}
	return {};
}

bool EncodeOgg(const std::vector<float>& theSamples, size_t theLoopStart, const std::string& thePath)
{
	FILE* aOut = fopen(thePath.c_str(), "wb");
	if (!aOut)
		return false;

	vorbis_info vi;
	vorbis_info_init(&vi);
	if (vorbis_encode_init_vbr(&vi, 1, kSampleRate, 0.4f) != 0)
		return false;
	vorbis_comment vc;
	vorbis_comment_init(&vc);
	vorbis_comment_add_tag(&vc, "ENCODER", "PvZ-Portable tools/wii-music");
	vorbis_comment_add_tag(&vc, "LOOPSTART", std::to_string(theLoopStart).c_str());
	vorbis_comment_add_tag(&vc, "LOOPEND", std::to_string(theSamples.size()).c_str());
	vorbis_dsp_state vd;
	vorbis_block vb;
	vorbis_analysis_init(&vd, &vi);
	vorbis_block_init(&vd, &vb);
	ogg_stream_state os;
	ogg_stream_init(&os, 1);

	ogg_packet aHeader, aComment, aCode;
	vorbis_analysis_headerout(&vd, &vc, &aHeader, &aComment, &aCode);
	ogg_stream_packetin(&os, &aHeader);
	ogg_stream_packetin(&os, &aComment);
	ogg_stream_packetin(&os, &aCode);
	ogg_page og;
	while (ogg_stream_flush(&os, &og))
	{
		fwrite(og.header, 1, og.header_len, aOut);
		fwrite(og.body, 1, og.body_len, aOut);
	}

	size_t aDone = 0;
	bool anEos = false;
	while (!anEos)
	{
		size_t aChunk = std::min<size_t>(4096, theSamples.size() - aDone);
		if (aChunk > 0)
		{
			float** aBuf = vorbis_analysis_buffer(&vd, (int)aChunk);
			memcpy(aBuf[0], &theSamples[aDone], aChunk * sizeof(float));
			aDone += aChunk;
		}
		vorbis_analysis_wrote(&vd, (int)aChunk); // 0 signals end of stream

		while (vorbis_analysis_blockout(&vd, &vb) == 1)
		{
			vorbis_analysis(&vb, nullptr);
			vorbis_bitrate_addblock(&vb);
			ogg_packet op;
			while (vorbis_bitrate_flushpacket(&vd, &op))
			{
				ogg_stream_packetin(&os, &op);
				while (!anEos && ogg_stream_pageout(&os, &og))
				{
					fwrite(og.header, 1, og.header_len, aOut);
					fwrite(og.body, 1, og.body_len, aOut);
					anEos = ogg_page_eos(&og);
				}
			}
		}
	}

	ogg_stream_clear(&os);
	vorbis_block_clear(&vb);
	vorbis_dsp_clear(&vd);
	vorbis_comment_clear(&vc);
	vorbis_info_clear(&vi);
	fclose(aOut);
	return true;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc != 3)
	{
		fprintf(stderr, "usage: %s <main.pak> <output dir>\n", argv[0]);
		return 1;
	}

	std::vector<char> aModule = ReadFromPak(argv[1], "sounds/mainmusic.mo3");
	if (aModule.empty())
	{
		fprintf(stderr, "couldn't find sounds/mainmusic.mo3 in %s\n", argv[1]);
		return 1;
	}

	for (const Tune& aTune : kTunes)
	{
		openmpt::module_ext aMod(aModule.data(), aModule.size());
		auto* anInteractive = static_cast<openmpt::ext::interactive*>(
			aMod.get_interface(openmpt::ext::interactive_id));
		for (int c = aTune.mMainEnd + 1; c < aMod.get_num_channels(); c++)
			anInteractive->set_channel_mute_status(c, true);

		// repeat_count 0 (the default): read() returns 0 once playback loops
		// back to a row it has already played, i.e. after exactly one loop.
		aMod.set_position_order_row(aTune.mStartOrder, 0);

		// Small chunks, so we know (to ~3 ms) where each row first started;
		// that's where the loop point goes once we see where it jumps back to.
		std::vector<float> aSamples;
		std::vector<float> aBuf(64);
		std::map<int, size_t> aFirstSampleOfRow;
		auto aRowKey = [&]() { return aMod.get_current_order() * 4096 + aMod.get_current_row(); };
		while (aSamples.size() < kMaxSeconds * kSampleRate)
		{
			aFirstSampleOfRow.emplace(aRowKey(), aSamples.size());
			size_t aCount = aMod.read(kSampleRate, aBuf.size(), aBuf.data());
			if (aCount == 0)
				break;
			aSamples.insert(aSamples.end(), aBuf.begin(), aBuf.begin() + aCount);
		}
		// Playback stopped because it jumped back to an already-played row:
		// the current position is that row.
		auto aLoopTarget = aFirstSampleOfRow.find(aRowKey());
		size_t aLoopStart = aLoopTarget != aFirstSampleOfRow.end() ? aLoopTarget->second : 0;

		std::string aPath = std::string(argv[2]) + "/" + aTune.mFile + ".ogg";
		if (!EncodeOgg(aSamples, aLoopStart, aPath))
		{
			fprintf(stderr, "failed to write %s\n", aPath.c_str());
			return 1;
		}
		printf("%-16s order 0x%02X  %6.1f s, loops back to order 0x%02X row %d at %.2f s  -> %s\n",
			aTune.mFile, aTune.mStartOrder, aSamples.size() / (double)kSampleRate,
			aMod.get_current_order(), aMod.get_current_row(), aLoopStart / (double)kSampleRate, aPath.c_str());
	}
	return 0;
}
