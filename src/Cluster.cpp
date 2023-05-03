/*
 * Copyright © 2017-2023 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
*/

#include <AudioEngine.h>
#include <Cluster.h>
#include "uart.h"
#include "Sample.h"
#include "functions.h"
#include "SampleManager.h"
#include <string.h>
#include "SampleCache.h"
#include "numericdriver.h"

Cluster::Cluster() {
	sample = NULL;
	chunkIndex = 0;
	sampleCache = NULL;
	extraBytesAtStartConverted = false;
	extraBytesAtEndConverted = false;
	loaded = false;
	numReasonsHeldBySampleRecorder = 0;
	numReasonsToBeLoaded = 0;
	// type is not set here, set it yourself (can't remember exact reason...)
}

void Cluster::convertDataIfNecessary() {

	if (!sample->audioDataStartPosBytes) return; // Or maybe we haven't yet figured out where the audio data starts

	if (sample->rawDataFormat) {

		memcpy(firstThreeBytesPreDataConversion, data, 3);

		int startPos = sample->audioDataStartPosBytes;
		int startChunk = startPos >> sampleManager.clusterSizeMagnitude;

		if (chunkIndex < startChunk) { // Hmm, there must have been a case where this happens...
			return;
		}

		// Special case for 24-bit with its uneven number of bytes
		if (sample->rawDataFormat == RAW_DATA_ENDIANNESS_WRONG_24) {
			char* pos;

			if (chunkIndex == startChunk) {
				pos = &data[startPos & (sampleManager.clusterSize - 1)];
			}
			else {
				uint32_t bytesBeforeStartOfChunk = chunkIndex * sampleManager.clusterSize - sample->audioDataStartPosBytes;
				int bytesThatWillBeEatingIntoAnother3Byte = bytesBeforeStartOfChunk % 3;
				if (bytesThatWillBeEatingIntoAnother3Byte == 0) bytesThatWillBeEatingIntoAnother3Byte = 3;
				pos = &data[3 - bytesThatWillBeEatingIntoAnother3Byte];
			}

			char const* endPos;
			if (chunkIndex == sample->getFirstChunkIndexWithNoAudioData() - 1) {
				uint32_t endAtBytePos = sample->audioDataStartPosBytes + sample->audioDataLengthBytes;
				uint32_t endAtPosWithinChunk = endAtBytePos & (sampleManager.clusterSize - 1);
				endPos = &data[endAtPosWithinChunk];
			}
			else {
				endPos = &data[sampleManager.clusterSize - 2];
			}

			while (true) {
				char const* endPosNow = pos + 1024; // Every this many bytes, we'll pause and do an audio routine
				if (endPosNow > endPos) endPosNow = endPos;

				while (pos < endPosNow) {
					uint8_t temp = pos[0];
					pos[0] = pos[2];
					pos[2] = temp;
					pos += 3;
				}

				if (pos >= endPos) break;

				AudioEngine::logAction("from convert-data");
				AudioEngine::routine(); // ----------------------------------------------------
			}
		}


		// Or, all other bit depths
		else {
			int32_t* pos;

			if (chunkIndex == startChunk) {
				pos = (int32_t*)&data[startPos & (sampleManager.clusterSize - 1)];
			}
			else {
				pos = (int32_t*)&data[startPos & 0b11];
			}

			int32_t* endPos;
			if (chunkIndex == sample->getFirstChunkIndexWithNoAudioData() - 1) {
				uint32_t endAtBytePos = sample->audioDataStartPosBytes + sample->audioDataLengthBytes;
				uint32_t endAtPosWithinChunk = endAtBytePos & (sampleManager.clusterSize - 1);
				endPos = (int32_t*)&data[endAtPosWithinChunk];
			}
			else {
				endPos = (int32_t*)&data[sampleManager.clusterSize - 3];
			}

			//uint16_t startTime = MTU2.TCNT_0;

			for (; pos < endPos; pos++) {

				if (!((uint32_t)pos & 0b1111111100)) {
					AudioEngine::logAction("from convert-data");
					AudioEngine::routine(); // ----------------------------------------------------
				}

				sample->convertOneData(pos);
			}

			/*
			uint16_t endTime = MTU2.TCNT_0;

			if (chunkIndex != startChunk) {
				Uart::print("time to convert: ");
				Uart::println((uint16_t)(endTime - startTime));
			}
			*/
		}
	}
}

int Cluster::getAppropriateQueue() {
	int q;

	// If it's a perc cache...
	if (type == LOADED_SAMPLE_CHUNK_PERC_CACHE_FORWARDS || type == LOADED_SAMPLE_CHUNK_PERC_CACHE_REVERSED) {
		q = sample->numReasons ?
				STEALABLE_QUEUE_CURRENT_SONG_SAMPLE_DATA_PERC_CACHE :
				STEALABLE_QUEUE_NO_SONG_SAMPLE_DATA_PERC_CACHE;
	}

	// If it's a regular repitched cache...
	else if (sampleCache) {
		q = (sampleCache->sample->numReasons) ?
			STEALABLE_QUEUE_CURRENT_SONG_SAMPLE_DATA_REPITCHED_CACHE :
			STEALABLE_QUEUE_NO_SONG_SAMPLE_DATA_REPITCHED_CACHE;
	}

	// Or, if it has a Sample...
	else if (sample) {
		q = sample->numReasons ?
				STEALABLE_QUEUE_CURRENT_SONG_SAMPLE_DATA :
				STEALABLE_QUEUE_NO_SONG_SAMPLE_DATA;

		if (sample->rawDataFormat) q++;
	}

	return q;
}


void Cluster::steal(char const* errorCode) {

	// Ok, we're now gonna decide what to do according to the actual "type" field for this Cluster.
	switch(type) {

	case LOADED_SAMPLE_CHUNK_SAMPLE:
		if (ALPHA_OR_BETA_VERSION && !sample) numericDriver.freezeWithError("E181");
		sample->clusters.getElement(chunkIndex)->loadedSampleChunk = NULL;
		break;

	case LOADED_SAMPLE_CHUNK_SAMPLE_CACHE:
		if (ALPHA_OR_BETA_VERSION && !sampleCache) numericDriver.freezeWithError("E183");
		sampleCache->chunkStolen(chunkIndex);

		// If first chunk, delete whole cache. Wait, no, something might still be pointing to the cache...
		/*
		if (!chunkIndex) {
			sampleCache->sample->deleteCache(sampleCache);
		}
		*/
		break;

	case LOADED_SAMPLE_CHUNK_PERC_CACHE_FORWARDS:
	case LOADED_SAMPLE_CHUNK_PERC_CACHE_REVERSED:
		if (ALPHA_OR_BETA_VERSION && !sample) numericDriver.freezeWithError("E184");
		sample->percCacheChunkStolen(this);
		break;

	default: // Otherwise, nothing needs to happen
		break;
	}
}

bool Cluster::mayBeStolen(void* thingNotToStealFrom) {
	if (numReasonsToBeLoaded) return false;

	if (!thingNotToStealFrom) return true;

	switch(type) {
	case LOADED_SAMPLE_CHUNK_SAMPLE_CACHE:
		return (sampleCache != thingNotToStealFrom);

	case LOADED_SAMPLE_CHUNK_PERC_CACHE_FORWARDS:
	case LOADED_SAMPLE_CHUNK_PERC_CACHE_REVERSED:
		return (sample != thingNotToStealFrom);
	}
	return true;
}
