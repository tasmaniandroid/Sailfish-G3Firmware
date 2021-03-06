/*
 * Copyright 2010 by Adam Mayer <adam@makerbot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include "SDCard.hh"

#include <avr/io.h>
#include <string.h>
#include "lib_sd/sd-reader_config.h"
#include "lib_sd/fat.h"
#include "lib_sd/sd_raw.h"
#include "lib_sd/partition.h"
#include "Motherboard.hh"
#include "Eeprom.hh"
#include "EepromMap.hh"
#include "EepromDefaults.hh"

#ifndef USE_DYNAMIC_MEMORY
#error Dynamic memory should be explicitly disabled in the G3 mobo.
#endif

#if (USE_DYNAMIC_MEMORY == 1)
#error Dynamic memory should be explicitly disabled in the G3 mobo.
#endif

namespace sdcard {

#ifndef BROKEN_SD
volatile bool mustReinit = true;
#else
static bool mustReinit = false;
#endif

uint8_t sdErrno;
SdErrorCode sdAvailable = SD_ERR_NO_CARD_PRESENT;

static struct partition_struct *partition = 0;
static struct fat_fs_struct *fs = 0;
static struct fat_dir_struct *cwd = 0; // current working directory
static struct fat_file_struct *file = 0;

void forceReinit() {
#ifndef BROKEN_SD
	mustReinit = true;
#endif
}

static bool openPartition()
{
  /* open first partition */
  partition = partition_open(sd_raw_read,
                             sd_raw_read_interval,
                             sd_raw_write,
                             sd_raw_write_interval,
                             0);

  if(!partition)
  {
    /* If the partition did not open, assume the storage device
    * is a "superfloppy", i.e. has no MBR.
    */
    partition = partition_open(sd_raw_read,
                               sd_raw_read_interval,
                               sd_raw_write,
                               sd_raw_write_interval,
                               -1);
  }
  return partition != 0;
}

static bool openFilesys()
{
  /* open file system */
  fs = fat_open(partition);
  return fs != 0;
}


static SdErrorCode changeWorkingDir(struct fat_dir_entry_struct *newDir)
{
	if ( !newDir ) {
		// Open the root directory
		struct fat_dir_entry_struct rootdirectory;
		fat_get_dir_entry_of_path(fs, "/", &rootdirectory);
		cwd = fat_open_dir(fs, &rootdirectory);
		return cwd ? SD_SUCCESS : SD_ERR_NO_ROOT;
	}

	struct fat_dir_struct *tmp = fat_open_dir(fs, newDir);
	if ( !tmp )
		return SD_ERR_FILE_NOT_FOUND;

	fat_close_dir(cwd);
	cwd = tmp;

	return SD_SUCCESS;
}

static SdErrorCode initCard() {
        uint8_t err;
	SdErrorCode sderr;

#ifndef BROKEN_SD
	reset();
#endif
	// Only accept a value of 1 as being ON.  This prevents a random EEPROM
	// setting from being interpreted as ON and then the user seeing potentially
	// degraded performance on an upgrade.  (Now they have to just happen to have
	// a value of 0x01 there to have this on accidentally after an upgrade.)
	if ( (err = sd_raw_init(eeprom::getEeprom8(eeprom::SD_USE_CRC,
						   EEPROM_DEFAULT_SD_USE_CRC) == 1)) ) {
		if ( openPartition() ) {
			if ( openFilesys() ) {
				if ( changeWorkingDir(0) == SD_SUCCESS ) {
					mustReinit = false;
					sdErrno = 0;
					sdAvailable = SD_SUCCESS;
					return SD_SUCCESS;
				}
				else sderr = SD_ERR_NO_ROOT;
			}
			else sderr = SD_ERR_OPEN_FILESYSTEM;
		}
		else sderr = SD_ERR_PARTITION_READ;
	}
	else {
	    if ( sd_errno == SDR_ERR_CRC )
		sderr = SD_ERR_CRC;
	    else
		sderr = sd_raw_available() ? SD_ERR_INIT_FAILED : SD_ERR_NO_CARD_PRESENT;
	}

	// Close the partition, file system, etc.
	reset();

	// reset() call initializes sdAvailable
	sdErrno = fat_errno;
	sdAvailable = sderr;

	return sderr;
}

SdErrorCode directoryReset() {
#ifdef BROKEN_SD
    reset();
    SdErrorCode rsp = initCard();
    if ( rsp != SD_SUCCESS && rsp != SD_ERR_CARD_LOCKED )
	return rsp;
#else
    if ( mustReinit ) {
	SdErrorCode rsp = initCard();
	if ( rsp != SD_SUCCESS )
	    return rsp;
    }
#endif
    fat_reset_dir(cwd);
    return SD_SUCCESS;
}

void directoryNextEntry(char* buffer, uint8_t bufsize, bool *isDir) {
	struct fat_dir_entry_struct entry;

        // In the original MBI consuming code, the end of the file list
	// was signalled by a return of an empty string.  So, we need to
	// always set the string's first byte to NUL before returning
	// an error.

	// This also makes the use of an error return pointless

	buffer[0] = 0; // assumes buffer != 0 && bufsize > 0

	uint8_t mask = FAT_ATTRIB_HIDDEN | FAT_ATTRIB_SYSTEM | FAT_ATTRIB_VOLUME;
	if (isDir) *isDir = false;
	else mask |= FAT_ATTRIB_DIR;

	if ( mustReinit && initCard() != SD_SUCCESS )
		return;

	// This is a bit of a hack.  For whatever reason, some filesystems return
	// files with nulls as the first character of their name.  This isn't
	// necessarily broken in of itself, but a null name is also our way
	// of signalling we've gone through the directory, so we discard these
	// entries.  We have an upper limit on the number of entries to cycle
	// through, so we don't potentially lock up here.

	uint8_t tries = 5;
	bufsize--;  // Assumes bufsize > 0
	while (tries) {

		if (!fat_read_dir(cwd, &entry))
			break;
		//Ignore non-file, system or hidden files
		if ( entry.attributes & mask )
			continue;
		if ( entry.long_name[0] ) {
			uint8_t i;
			if ( entry.attributes & FAT_ATTRIB_DIR )
				*isDir = true;
			for (i = 0; i < bufsize && entry.long_name[i] != 0; i++)
				buffer[i] = entry.long_name[i];
			buffer[i] = 0;
			return;
		}
		--tries;
	}
}

static bool findFileInDir(const char* name, struct fat_dir_entry_struct* dir_entry)
{
	fat_reset_dir(cwd);
	while ( fat_read_dir(cwd, dir_entry) )
		if ( strcmp(dir_entry->long_name, name) == 0 )
			return true;
	return false;
}

bool changeDirectory(const char* dname)
{
	struct fat_dir_entry_struct dirEntry;

	if ( mustReinit ) return false;

	if ( !findFileInDir(dname, &dirEntry) || !(dirEntry.attributes & FAT_ATTRIB_DIR) )
		return false;

	return ( changeWorkingDir(&dirEntry) == SD_SUCCESS );
}

static void finishFile() {
	if ( file == 0 )
		return;
	fat_close_file(file);
	sd_raw_sync();
	file = 0;
}

// WARNING: if the file is a directory, we merely move into it
// Return values:
//   0 -- Error
//  +1 -- File opened
//  -1 -- Moved to the directory; file not opened

static int8_t openFile(const char* name)
{
	struct fat_dir_entry_struct fileEntry;

	if ( !findFileInDir(name, &fileEntry) )
		return 0;

	if ( fileEntry.attributes & FAT_ATTRIB_DIR )
		return ( changeWorkingDir(&fileEntry) == SD_SUCCESS ) ? -1 : 0;

	finishFile();
	file = fat_open_file(fs, &fileEntry);
	return (file != 0) ? 1 : 0;
}

static void deleteFile(char *name)
{
	struct fat_dir_entry_struct fileEntry;

	if ( findFileInDir(name, &fileEntry) )
		fat_delete_file(fs, &fileEntry);
}

static bool createFile(char *name)
{
	struct fat_dir_entry_struct fileEntry;

	return fat_create_file(cwd, name, &fileEntry) != 0;
}

static bool capturing = false;
static bool playing = false;
static int32_t fileSizeBytes = 0L;
static int32_t playedBytes = 0L;
static uint32_t capturedBytes = 0L;

bool isPlaying() {
	return playing;
}

bool isCapturing() {
	return capturing;
}

SdErrorCode startCapture(char* filename)
{
#ifndef BROKEN_SD
    if ( mustReinit ) {
	SdErrorCode rsp = initCard();
	if ( rsp != SD_SUCCESS ) return rsp;
    }
#else
    reset();
    SdErrorCode result = initCard();
    if ( result != SD_SUCCESS )
	return result;
#endif

    if ( sd_raw_locked() )
	return SD_ERR_CARD_LOCKED;

    capturedBytes = 0L;
    playedBytes = 0L;

    // Always operate in truncation mode.
    deleteFile(filename);
    if ( !createFile(filename) )
	return SD_ERR_FILE_NOT_FOUND;
    
    if ( openFile(filename) != 1 )
	return SD_ERR_GENERIC;

    capturing = true;
    return SD_SUCCESS;
}

void capturePacket(const Packet& packet)
{
	if (file == 0) return;
	// Casting away volatile is OK in this instance; we know where the
	// data is located and that fat_write_file isn't caching
	fat_write_file(file, (uint8_t*)packet.getData(), packet.getLength());
	capturedBytes += packet.getLength();
}

#ifdef EEPROM_MENU_ENABLE

/// Writes b to the open file
void writeByte(uint8_t b) {
	fat_write_file(file, (uint8_t *)&b, (uintptr_t)1);
}

#endif

uint32_t finishCapture()
{
	if ( capturing ) {
		finishFile();
		capturing = false;
	}
	return capturedBytes;
}

static uint8_t next_byte;
static bool has_more = false;

void fetchNextByte() {

    // BE WARNED: fat_read_file() only returns an error on the first
    //   call which encounters the error.  The next call after the error
    //   return will merely return 0 (no bytes read).

    int16_t read = fat_read_file(file, &next_byte, 1);
    playedBytes++;
    if ( read > 0 )
	return;
    else {
	has_more = false;
	if ( read < 0 ) {
	    if ( !sd_raw_available() )
		sdAvailable = SD_ERR_NO_CARD_PRESENT;
	    else
		sdAvailable = ( fat_errno == FAT_ERR_CRC ) ? SD_ERR_CRC : SD_ERR_READ;
	}
    }
}

bool playbackHasNext() {
    return has_more;
}

uint8_t playbackNext() {
    uint8_t rv = next_byte;
    fetchNextByte();
    return rv;
}

SdErrorCode startPlayback(char* filename) {
#ifndef BROKEN_SD
    if ( mustReinit ) {
	SdErrorCode rsp = initCard();
	if ( rsp != SD_SUCCESS ) return rsp;
    }
#else
    reset();
    SdErrorCode result = initCard(); 
    if ( result != SD_SUCCESS && result != SD_ERR_CARD_LOCKED )
	return result;
#endif

    capturedBytes = 0L;
    playedBytes = 0L;

    int8_t res = openFile(filename);
    if ( res == 0 )
	return SD_ERR_FILE_NOT_FOUND;
    else if ( res == -1 )
	// The file was a directory and we successfully moved into it
	return SD_CWD;

    playing = true;

    int32_t off = 0L;
    fat_seek_file(file, &off, FAT_SEEK_END);
    fileSizeBytes = off;
    off = 0L;
    fat_seek_file(file, &off, FAT_SEEK_SET);

    Motherboard::getBoard().resetCurrentSeconds();
    has_more = true;
    fetchNextByte();
    return SD_SUCCESS;
}

void playbackRestart() {
	int32_t offset = 0;	
	fat_seek_file(file, &offset, FAT_SEEK_SET);

	capturedBytes = 0L;
	playedBytes = 0L;
	playing = true;
	has_more = true;

	fetchNextByte();
}

float getPercentPlayed() {
  float percentPlayed = (float)(playedBytes * 100) / (float)fileSizeBytes;

  if      ( percentPlayed > 100.0 )	return 100.0;
  else if ( percentPlayed < 0.0 )	return 0.0;
  else					return percentPlayed;
}


void finishPlayback() {
	if ( !playing ) return;
	finishFile();
	playing = false;
	has_more = false;
}


void reset() {
	finishPlayback();
	finishCapture();
	finishFile();
	if (cwd != 0) {
		fat_close_dir(cwd);
		cwd = 0;
	}
	if (fs != 0) {
		fat_close(fs);
		fs = 0;
	}
	if (partition != 0) {
		partition_close(partition);
		partition = 0;
	}
#ifndef BROKEN_SD
	mustReinit = true;
#endif
	sdAvailable = SD_ERR_NO_CARD_PRESENT;
}

/// Return true if file name exists on the SDCard
bool fileExists(const char* name)
{
  struct fat_dir_entry_struct fileEntry;
	
  if ( directoryReset() != SD_SUCCESS )
	  return false;

  return findFileInDir(name, &fileEntry);
}

} // namespace sdcard
