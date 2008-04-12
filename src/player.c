/* the Music Player Daemon (MPD)
 * Copyright (C) 2003-2007 by Warren Dukes (warren.dukes@gmail.com)
 * This project's homepage is: http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "player.h"
#include "path.h"
#include "decode.h"
#include "command.h"
#include "interface.h"
#include "playlist.h"
#include "ls.h"
#include "listen.h"
#include "log.h"
#include "utils.h"
#include "directory.h"
#include "volume.h"
#include "playerData.h"
#include "permission.h"
#include "sig_handlers.h"
#include "os_compat.h"

static pthread_cond_t player_wakeup = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t player_wakeup_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t main_wakeup = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t main_wakeup_mutex = PTHREAD_MUTEX_INITIALIZER;

static void playerCloseAudio(void);

void wakeup_player_nb(void)
{
	pthread_cond_signal(&player_wakeup);
}

static void wakeup_player(void)
{
	pthread_cond_signal(&player_wakeup);
	pthread_cond_wait(&main_wakeup, &main_wakeup_mutex);
}

void wakeup_main_task(void)
{
	pthread_cond_signal(&main_wakeup);
}

void player_sleep(void)
{
	pthread_cond_wait(&player_wakeup, &player_wakeup_mutex);
}

static void * player_task(mpd_unused void *unused)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	while (1) {
		if (pc->play) {
			decode();
			continue; /* decode() calls wakeup_main_task */
		} else if (pc->stop) {
			pc->stop = 0;
		} else if (pc->seek) {
			pc->seek = 0;
		} else if (pc->pause) {
			pc->pause = 0;
		} else if (pc->closeAudio) {
			closeAudioDevice();
			pc->closeAudio = 0;
		} else if (pc->lockQueue) {
			pc->queueLockState = PLAYER_QUEUE_LOCKED;
			pc->lockQueue = 0;
		} else if (pc->unlockQueue) {
			pc->queueLockState = PLAYER_QUEUE_UNLOCKED;
			pc->unlockQueue = 0;
		} else {
			player_sleep();
			continue;
		}
		/* we did something, tell the main task about it */
		wakeup_main_task();
	}
	return NULL;
}

static void resetPlayerMetadata(void)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	if (pc->metadataState == PLAYER_METADATA_STATE_READ) {
		pc->metadataState = PLAYER_METADATA_STATE_WRITE;
	}
}

void playerInit(void)
{
	pthread_attr_t attr;
	pthread_t player_thread;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&player_thread, &attr, player_task, NULL))
		FATAL("Failed to spawn player task: %s\n", strerror(errno));
}

int playerWait(int fd)
{
	if (playerStop(fd) < 0)
		return -1;

	playerCloseAudio();

	return 0;
}

static void set_current_song(Song *song)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	pc->fileTime = song->tag ? song->tag->time : 0;
	copyMpdTagToMetadataChunk(song->tag, &(pc->fileMetadataChunk));
	get_song_url(pc->utf8url, song);
}

int playerPlay(int fd, Song * song)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	if (playerStop(fd) < 0)
		return -1;

	set_current_song(song);

	resetPlayerMetadata();
	pc->play = 1;
	/* FIXME: _nb() variant is probably wrong here, and everywhere... */
	do { wakeup_player_nb(); } while (pc->play);

	return 0;
}

int playerStop(int fd)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	if (pc->state != PLAYER_STATE_STOP) {
		pc->stop = 1;
		do { wakeup_player(); } while (pc->stop);
	}

	pc->queueState = PLAYER_QUEUE_BLANK;
	playerQueueUnlock();

	return 0;
}

void playerKill(void) /* deprecated */
{
	playerPause(STDERR_FILENO);
}

int playerPause(int fd)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	if (pc->state != PLAYER_STATE_STOP) {
		pc->pause = 1;
		do { wakeup_player(); } while (pc->pause);
	}

	return 0;
}

int playerSetPause(int fd, int pause_flag)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	switch (pc->state) {
	case PLAYER_STATE_PLAY:
		if (pause_flag)
			playerPause(fd);
		break;
	case PLAYER_STATE_PAUSE:
		if (!pause_flag)
			playerPause(fd);
		break;
	}

	return 0;
}

int getPlayerElapsedTime(void)
{
	return (int)(getPlayerData()->playerControl.elapsedTime + 0.5);
}

unsigned long getPlayerBitRate(void)
{
	return getPlayerData()->playerControl.bitRate;
}

int getPlayerTotalTime(void)
{
	return (int)(getPlayerData()->playerControl.totalTime + 0.5);
}

int getPlayerState(void)
{
	return getPlayerData()->playerControl.state;
}

void clearPlayerError(void)
{
	getPlayerData()->playerControl.error = 0;
}

int getPlayerError(void)
{
	return getPlayerData()->playerControl.error;
}

char *getPlayerErrorStr(void)
{
	static char *error;
	int errorlen = MPD_PATH_MAX + 1024;
	PlayerControl *pc = &(getPlayerData()->playerControl);

	error = xrealloc(error, errorlen);
	error[0] = '\0';

	switch (pc->error) {
	case PLAYER_ERROR_FILENOTFOUND:
		snprintf(error, errorlen,
			 "file \"%s\" does not exist or is inaccessible",
			 pc->erroredUrl);
		break;
	case PLAYER_ERROR_FILE:
		snprintf(error, errorlen, "problems decoding \"%s\"",
			 pc->erroredUrl);
		break;
	case PLAYER_ERROR_AUDIO:
		snprintf(error, errorlen, "problems opening audio device");
		break;
	case PLAYER_ERROR_SYSTEM:
		snprintf(error, errorlen, "system error occured");
		break;
	case PLAYER_ERROR_UNKTYPE:
		snprintf(error, errorlen, "file type  of \"%s\" is unknown",
			 pc->erroredUrl);
	default:
		break;
	}

	errorlen = strlen(error);
	error = xrealloc(error, errorlen + 1);

	if (errorlen)
		return error;

	return NULL;
}

static void playerCloseAudio(void)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	if (playerStop(STDERR_FILENO) < 0)
		return;
	pc->closeAudio = 1;
	do { wakeup_player(); } while (pc->closeAudio);
}

int queueSong(Song * song)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	if (pc->queueState == PLAYER_QUEUE_BLANK) {
		set_current_song(song);
		pc->queueState = PLAYER_QUEUE_FULL;
		return 0;
	}

	return -1;
}

int getPlayerQueueState(void)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	return pc->queueState;
}

void setQueueState(int queueState)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	pc->queueState = queueState;
}

void playerQueueLock(void)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	if (pc->queueLockState == PLAYER_QUEUE_UNLOCKED) {
		pc->lockQueue = 1;
		do { wakeup_player(); } while (pc->lockQueue);
	}
}

void playerQueueUnlock(void)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	if (pc->queueLockState == PLAYER_QUEUE_LOCKED) {
		pc->unlockQueue = 1;
		do { wakeup_player(); } while (pc->unlockQueue);
	}
}

int playerSeek(int fd, Song * song, float seek_time)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);
	char path_max_tmp[MPD_PATH_MAX];

	assert(song != NULL);

	if (pc->state == PLAYER_STATE_STOP) {
		commandError(fd, ACK_ERROR_PLAYER_SYNC,
			     "player not currently playing");
		return -1;
	}

	if (strcmp(pc->utf8url, get_song_url(path_max_tmp, song)) != 0)
		set_current_song(song);

	if (pc->error == PLAYER_ERROR_NOERROR) {
		resetPlayerMetadata();
		pc->seekWhere = seek_time;
		pc->seek = 1;
		/* FIXME: _nb() is probably wrong here, too */
		do { wakeup_player_nb(); } while (pc->seek);
	}

	return 0;
}

float getPlayerCrossFade(void)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	return pc->crossFade;
}

void setPlayerCrossFade(float crossFadeInSeconds)
{
	PlayerControl *pc;
	if (crossFadeInSeconds < 0)
		crossFadeInSeconds = 0;

	pc = &(getPlayerData()->playerControl);

	pc->crossFade = crossFadeInSeconds;
}

void setPlayerSoftwareVolume(int volume)
{
	PlayerControl *pc;
	volume = (volume > 1000) ? 1000 : (volume < 0 ? 0 : volume);

	pc = &(getPlayerData()->playerControl);

	pc->softwareVolume = volume;
}

double getPlayerTotalPlayTime(void)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	return pc->totalPlayTime;
}

unsigned int getPlayerSampleRate(void)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	return pc->sampleRate;
}

int getPlayerBits(void)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	return pc->bits;
}

int getPlayerChannels(void)
{
	PlayerControl *pc = &(getPlayerData()->playerControl);

	return pc->channels;
}

/* this actually creates a dupe of the current metadata */
Song *playerCurrentDecodeSong(void)
{
	static Song *song;
	static MetadataChunk *prev;
	Song *ret = NULL;
	PlayerControl *pc = &(getPlayerData()->playerControl);

	if (pc->metadataState == PLAYER_METADATA_STATE_READ) {
		if (prev)
			free(prev);
		prev = xmalloc(sizeof(MetadataChunk));
		memcpy(prev, &(pc->metadataChunk), sizeof(MetadataChunk));
		if (song)
			freeJustSong(song);
		song = newNullSong();
		song->url = xstrdup(pc->currentUrl);
		song->tag = metadataChunkToMpdTagDup(prev);
		ret = song;
		resetPlayerMetadata();
	}

	return ret;
}
