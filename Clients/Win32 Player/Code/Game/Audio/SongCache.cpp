#include "Game/Audio/SongCache.hpp"
#include "ThirdParty/fmod/fmod.h"
#include "ThirdParty/fmod/fmod.hpp"
#include "Engine/Core/ErrorWarningAssert.hpp"
#include "Engine/Time/Time.hpp"
#include "Engine/Core/StringUtils.hpp"
#include "Engine/Input/Console.hpp"
#include "Engine/Core/JobSystem.hpp"
#include "../TheGame.hpp"

//-----------------------------------------------------------------------------------
void LoadSongJob(Job* job)
{
    SongResourceInfo* songResource = (SongResourceInfo*)job->data;
    RawSoundHandle song = nullptr;
    unsigned int errorValue = 0;

    while (song == nullptr)
    {
        song = AudioSystem::instance->LoadRawSound(songResource->m_filePath, errorValue);

       if (errorValue == 43)
        {
            Console::instance->PrintLine("ERROR: OUT OF MEMORY, CAN'T LOAD SONG", RGBA::RED);
            return;
        }
        ASSERT_RECOVERABLE(errorValue == 0 || errorValue == 19, "Hit an unexpected error code while loading a file");
    }

    songResource->m_songData = (void*)song;
}

//-----------------------------------------------------------------------------------
SongCache::SongCache()
{

}

//-----------------------------------------------------------------------------------
SongCache::~SongCache()
{

}

//-----------------------------------------------------------------------------------
SongID SongCache::RequestSongLoad(const std::wstring& filePath)
{
    //Load the song into memory completely if it can, otherwise we create the placeholders and load later
    SongID songID = SongCache::CalculateSongID(filePath);
    std::map<SongID, SongResourceInfo>::iterator found = m_songCache.find(songID);
    SongResourceInfo* songResourceInfo = nullptr;

    if (found != m_songCache.end())
    {
        songResourceInfo = &found->second;
        if (songResourceInfo->m_timeLastAccessedMS > -1 && !songResourceInfo->m_songData)
        {
            //If the track has been loaded and played before, and then unloaded, increase the cache size and load again
            m_cacheSizeBytes += GetFileSizeBytes(filePath);
        }
        else if (songResourceInfo->m_songData)
        {
            //The song ID is in the cache and the song is loaded, so we're able to play
            return songID;
        }
    }
    else
    {
        //Song ID wasn't found, so we need to either load the song if we have the space, or create a placeholder and load later
        m_songCache[songID].m_songID = songID; //Forcibly create a struct of info for the cache
        songResourceInfo = &m_songCache[songID];
        songResourceInfo->m_filePath = filePath;
        m_cacheSizeBytes += GetFileSizeBytes(filePath);

        //We want to make the placeholders for the song if we're over the memory threshhold. 
        if (GetFileSizeBytes(filePath) + m_cacheSizeBytes >= MAX_MEMORY_THRESHOLD && m_songCache.size() > 1)
        {
            return songID;
        }
    }

    JobSystem::instance->CreateAndDispatchJob(GENERIC_SLOW, &LoadSongJob, songResourceInfo);
    return songID;
}

//-----------------------------------------------------------------------------------
SongID SongCache::EnsureSongLoad(const std::wstring& filePath)
{
    //We need to load the song now, so we delete from the cache if necessary
    SongID songID = SongCache::CalculateSongID(filePath);
    std::map<SongID, SongResourceInfo>::iterator found = m_songCache.find(songID);
    SongResourceInfo* songResourceInfo = nullptr;

    if (found != m_songCache.end())
    {
        songResourceInfo = &found->second;
        if (songResourceInfo->m_timeLastAccessedMS > -1 && !songResourceInfo->m_songData)
        {
            m_cacheSizeBytes += GetFileSizeBytes(filePath);
        }
        else if (songResourceInfo->m_songData)
        {
            return songID;
        }
    }
    else
    {
        //We want to make the placeholders for the song if we're over the memory threshhold. 
        while (GetFileSizeBytes(filePath) + m_cacheSizeBytes >= MAX_MEMORY_THRESHOLD && m_songCache.size() > 1)
        {
            //Remove the least accessed songs until enough memory is available
            RemoveFromCache(FindLeastAccessedSong());
        }
        m_songCache[songID].m_songID = songID; //Forcibly create a struct of info for the cache
        songResourceInfo = &m_songCache[songID];
        songResourceInfo->m_filePath = filePath;
        m_cacheSizeBytes += GetFileSizeBytes(filePath);
    }
        JobSystem::instance->CreateAndDispatchJob(GENERIC_SLOW, &LoadSongJob, songResourceInfo);
        return songID;
}

//-----------------------------------------------------------------------------------
RawSoundHandle SongCache::RequestSoundHandle(const SongID songID)
{
    RawSoundHandle song = nullptr;

    std::map<SongID, SongResourceInfo>::iterator found = m_songCache.find(songID);
    if (found != m_songCache.end())
    {
        SongResourceInfo& info = found->second;
        song = info.m_songData;
        info.m_timeLastAccessedMS = GetCurrentTimeMilliseconds();
    }

    return song;
}

//-----------------------------------------------------------------------------------
bool SongCache::IsValid(const SongID songID)
{
    std::map<SongID, SongResourceInfo>::iterator found = m_songCache.find(songID);
    if (found != m_songCache.end())
    {
        SongResourceInfo& info = found->second;
        return info.IsValid();
    }

    return false;
}

//-----------------------------------------------------------------------------------
void SongCache::PrintErrorInConsole(const SongID songID)
{
    std::map<SongID, SongResourceInfo>::iterator found = m_songCache.find(songID);
    if (found != m_songCache.end())
    {
        SongResourceInfo& info = found->second;
        Console::instance->PrintLine(Stringf("AUDIO SYSTEM ERROR: Got error result code %d.\n", info.m_loadErrorCode), RGBA::RED);
    }
}

//-----------------------------------------------------------------------------------
size_t SongCache::CalculateSongID(const std::wstring& filePath)
{
    return std::hash<std::wstring>{}(filePath);
}

//-----------------------------------------------------------------------------------
void SongCache::Flush()
{
    m_songCache.clear();
}

//-----------------------------------------------------------------------------------
SongResourceInfo::~SongResourceInfo()
{
    AudioSystem::instance->ReleaseRawSong(m_songData);
}

//-----------------------------------------------------------------------------------
SongID SongCache::FindLeastAccessedSong()
{
    double lowestAccessTime = -1;
    SongID leastAccessedSong = 0;

    //Try to find a song that is loaded in and has been played. Otherwise find one that has been loaded
    for (std::map<SongID, SongResourceInfo>::iterator i = m_songCache.begin(); i != m_songCache.end(); ++i)
    {
        SongResourceInfo& info = i->second;
        //Has this song been loaded yet? Has it been loaded and not played? Is the last load time less than what we have?
        if (((info.m_timeLastAccessedMS < lowestAccessTime) || lowestAccessTime == -1) && info.m_timeLastAccessedMS != -1 && info.m_songData && !info.m_isPlaying)
        {
            lowestAccessTime = info.m_timeLastAccessedMS;
            leastAccessedSong = info.m_songID;
        }
        else if (info.m_songData && !info.m_isPlaying && leastAccessedSong == -1)
        {
            leastAccessedSong = info.m_songID;
        }
    }

    return leastAccessedSong;
}

//-----------------------------------------------------------------------------------
void SongCache::RemoveFromCache(const SongID songID)
{
    if (songID == 0)
    {
        return;
    }

    std::map<SongID, SongResourceInfo>::iterator found = m_songCache.find(songID);
    if (found != m_songCache.end())
    {
        SongResourceInfo& info = found->second;
        //If song is the one currently playing, don't delete it
        m_cacheSizeBytes -= GetFileSizeBytes(info.m_filePath);
        delete info.m_songData;
        info.m_songData = nullptr;
    }
    else
    {
        ASSERT_RECOVERABLE(found == m_songCache.end(), "Could not remove song from cache.\n");
    }
}

//-----------------------------------------------------------------------------------
void SongCache::UpdateLastAccessedTime(const SongID songID)
{
    SongResourceInfo& info = m_songCache.at(songID);
    info.m_timeLastAccessedMS = GetCurrentTimeMilliseconds();
}

//-----------------------------------------------------------------------------------
void SongCache::TogglePlayingStatus(const SongID songID)
{
    std::map<SongID, SongResourceInfo>::iterator found = m_songCache.find(songID);
    if (found != m_songCache.end())
    {
        SongResourceInfo& info = found->second;
        if (!info.m_isPlaying)
        {
            info.m_isPlaying = true;
        }
        else
        {
            info.m_isPlaying = false;
        }
    }
}