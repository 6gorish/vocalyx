#include "AudioFileLoader.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstdint>

namespace vocalyx {

namespace {
std::string statusText(const char* what, OSStatus status) {
    return std::string(what) + " (OSStatus " + std::to_string(static_cast<int>(status)) + ")";
}
} // namespace

bool loadAudioFile(const std::string& path,
                   std::vector<std::vector<float>>& channelsOut,
                   double& sampleRateOut,
                   std::string& errorOut) {
    channelsOut.clear();
    sampleRateOut = 0.0;
    errorOut.clear();

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(path.c_str()),
        static_cast<CFIndex>(path.size()),
        false);

    if (url == nullptr) {
        errorOut = "could not interpret the path";
        return false;
    }

    ExtAudioFileRef file = nullptr;
    OSStatus status = ExtAudioFileOpenURL(url, &file);
    CFRelease(url);

    if (status != noErr || file == nullptr) {
        errorOut = statusText("could not open the file", status);
        return false;
    }

    // --- what the file actually contains ---
    AudioStreamBasicDescription fileFormat{};
    UInt32 size = sizeof(fileFormat);
    status = ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat, &size, &fileFormat);

    if (status != noErr) {
        ExtAudioFileDispose(file);
        errorOut = statusText("could not read the file format", status);
        return false;
    }

    const int    channels   = static_cast<int>(fileFormat.mChannelsPerFrame);
    const double sampleRate = fileFormat.mSampleRate;

    if (channels < 1 || sampleRate <= 0.0) {
        ExtAudioFileDispose(file);
        errorOut = "the file reports no audio channels";
        return false;
    }

    // --- what we want it converted into ---
    // Non-interleaved 32-bit float at the file's own rate. AudioToolbox handles
    // the conversion, including decoding compressed formats.
    AudioStreamBasicDescription clientFormat{};
    clientFormat.mSampleRate       = sampleRate;
    clientFormat.mFormatID         = kAudioFormatLinearPCM;
    clientFormat.mFormatFlags      = kAudioFormatFlagIsFloat
                                   | kAudioFormatFlagIsPacked
                                   | kAudioFormatFlagIsNonInterleaved;
    clientFormat.mBitsPerChannel   = 32;
    clientFormat.mChannelsPerFrame = static_cast<UInt32>(channels);
    clientFormat.mFramesPerPacket  = 1;
    clientFormat.mBytesPerFrame    = sizeof(float);   // per channel, non-interleaved
    clientFormat.mBytesPerPacket   = sizeof(float);

    status = ExtAudioFileSetProperty(file,
                                     kExtAudioFileProperty_ClientDataFormat,
                                     sizeof(clientFormat),
                                     &clientFormat);

    if (status != noErr) {
        ExtAudioFileDispose(file);
        errorOut = statusText("could not set the conversion format", status);
        return false;
    }

    // --- length ---
    SInt64 totalFrames = 0;
    size = sizeof(totalFrames);
    status = ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileLengthFrames, &size, &totalFrames);

    if (status != noErr || totalFrames <= 0) {
        ExtAudioFileDispose(file);
        errorOut = "the file contains no frames";
        return false;
    }

    channelsOut.assign(static_cast<size_t>(channels),
                       std::vector<float>(static_cast<size_t>(totalFrames), 0.0f));

    // An AudioBufferList carries one AudioBuffer per channel in non-interleaved
    // mode, and the struct declares only the first, so the storage has to be
    // sized by hand.
    std::vector<std::uint8_t> listStorage(
        sizeof(AudioBufferList) + sizeof(AudioBuffer) * static_cast<size_t>(channels - 1));

    auto* bufferList = reinterpret_cast<AudioBufferList*>(listStorage.data());
    bufferList->mNumberBuffers = static_cast<UInt32>(channels);

    // --- read ---
    const SInt64 chunkFrames = 8192;
    SInt64 framesRead = 0;

    while (framesRead < totalFrames) {
        const UInt32 wanted = static_cast<UInt32>(std::min(chunkFrames, totalFrames - framesRead));

        for (int c = 0; c < channels; ++c) {
            bufferList->mBuffers[c].mNumberChannels = 1;
            bufferList->mBuffers[c].mDataByteSize   = wanted * sizeof(float);
            bufferList->mBuffers[c].mData           = channelsOut[static_cast<size_t>(c)].data() + framesRead;
        }

        UInt32 got = wanted;
        status = ExtAudioFileRead(file, &got, bufferList);

        if (status != noErr) {
            ExtAudioFileDispose(file);
            channelsOut.clear();
            errorOut = statusText("failed while reading audio", status);
            return false;
        }

        if (got == 0)
            break;   // end of file, which for compressed formats can arrive early

        framesRead += got;
    }

    ExtAudioFileDispose(file);

    if (framesRead == 0) {
        channelsOut.clear();
        errorOut = "no audio was decoded";
        return false;
    }

    // Compressed formats report an estimated length, so trim to what arrived.
    if (framesRead < totalFrames)
        for (auto& channel : channelsOut)
            channel.resize(static_cast<size_t>(framesRead));

    sampleRateOut = sampleRate;
    return true;
}

} // namespace vocalyx
