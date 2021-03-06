/*
 * Copyright (C) 2017 Vojtech Vladyka <vojtech.vladyka@gmail.com>
 * Copyright (C) 2019 Steven J. Magnani <magnani@ieee.org>
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
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <ctype.h>  // isxdigit()
#include <math.h>
#include <time.h>
#include <limits.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/param.h>

#include "udffsck.h"
#include "utils.h"
#include "libudffs.h"
#include "options.h"

// Local function prototypes
uint8_t get_file(udf_media_t *media,
                 uint32_t lsn, struct filesystemStats *stats, uint32_t depth,
                 uint32_t uuid, struct fileInfo info, vds_sequence_t *seq );
void increment_used_space(struct filesystemStats *stats, uint64_t increment, uint32_t position);
uint8_t inspect_fid(udf_media_t *media,
                    uint32_t lsn, uint8_t *base, uint32_t *pos,
                    struct filesystemStats *stats, uint32_t depth, vds_sequence_t *seq, uint8_t *status);
void print_file_chunks(struct filesystemStats *stats);
int copy_descriptor(udf_media_t *media,
                    uint32_t sourcePosition, uint32_t destinationPosition, size_t size);
int append_error(vds_sequence_t *seq, uint16_t tagIdent, vds_type_e vds, uint8_t error);
uint8_t get_error(vds_sequence_t *seq, uint16_t tagIdent, vds_type_e vds);
static void update_min_udf_revision(struct filesystemStats *stats, uint16_t new_revision);

// Local defines
#define MARK_BLOCK 1    ///< Mark switch for markUsedBlock() function
#define UNMARK_BLOCK 0  ///< Unmark switch for markUsedBlock() function

#define MAX_DEPTH 100 ///< Maximal printed filetree depth is MAX_DEPTH/4. Required by function depth2str().

/**
 * \brief File tree prefix creator
 *
 * This function takes depth and based on that prints lines and splits
 *
 * \param[in] depth required depth to print
 * \return NULL terminated static char array with printed depth
 */
char * depth2str(int32_t depth) {
    static char prefix[MAX_DEPTH] = {0};

    if(depth == 0) {
        return prefix;
    }

    if(depth < MAX_DEPTH) {
        int i=0, c=0;
        int width = 4;
        for(i=0, c=0; c<depth-1; c++, i+=width) {
            strcpy(prefix+i, "\u2502 ");
        }
        strcpy(prefix+i, "\u251C\u2500");
    }
    return prefix;
}

/**
 * \brief Checksum calculation function for tags
 *
 * This function is tailored for checksum calculation for UDF tags.
 * It skips 5th byte, because that's where the tag checksum is stored.
 *
 * \param[in] descTag target for checksum calculation
 * \return checksum result
 */
uint8_t calculate_checksum(tag descTag) {
    uint8_t i;
    uint8_t tagChecksum = 0;

    for (i=0; i<16; i++)
        if (i != 4)
            tagChecksum += (uint8_t)(((char *)&(descTag))[i]);

    return tagChecksum;
}

/**
 * \brief Wrapper function for checksum
 *
 * \param[in] descTag target for checksum calculation
 * \return result of checksum comparison, 1 if match, 0 if differs
 *
 * \warning This function has opposite result polarity vs. crc() and check_position()
 */
int checksum(tag descTag) {
    uint8_t checksum =  calculate_checksum(descTag);
    dbg("Calc checksum: 0x%02x Tag checksum: 0x%02x\n", checksum, descTag.tagChecksum);
    return checksum == descTag.tagChecksum;
}

/**
 * \brief CRC calculation wrapper for udf_crc() function from libudffs
 *
 * \param[in] desc descriptor for calculation
 * \param[in] size size for calculation
 * \return CRC result
 */
uint16_t calculate_crc(void * restrict desc, uint16_t size) {
    uint8_t offset = sizeof(tag);
    uint16_t crc = 0;

    if(size >= 16) {
        uint16_t calcCrc = udf_crc((uint8_t *)(desc) + offset, size - offset, crc);
        return calcCrc;
    } else {
        return 0;
    }
}

/**
 * \brief Wrapper function for CRC calculation
 *
 * \param[in] desc descriptor for calculation
 * \param[in] size size for calculation
 * \return result of checksum comparison, 0 if match, 1 if differs
 */
int crc(void * restrict desc, uint16_t size) {
    uint16_t calcCrc = calculate_crc(desc, size);
    tag *descTag = desc;
    dbg("Calc CRC: 0x%04x, TagCRC: 0x%04x\n", calcCrc, descTag->descCRC);
    return le16_to_cpu(descTag->descCRC) != calcCrc;
}

/**
 * \brief Position check function
 *
 * Checks declared position from tag against inserted position
 *
 * \param[in] descTag tag with declared position
 * \param[in] position actual position to compare
 * \return result of position comparison, 0 if match, 1 if differs
 */
int check_position(tag descTag, uint32_t position) {
    dbg("tag pos: 0x%x, pos: 0x%x\n", descTag.tagLocation, position);
    return (descTag.tagLocation != position);
}

/**
 * \brief Timestamp printing function
 *
 * This function prints timestamp to static char array in human readable form
 * 
 * Used format is YYYY-MM-DD hh:mm:ss.cshmms+hh:mm\n
 * cs -- centiseconds\n
 * hm -- hundreds of microseconds\n
 * ms -- microseconds\n
 *
 * \param[in] ts UDF timestamp
 * \return pointer to char static char array
 *
 * \warning char array is NOT NULL terminated
 */
char * print_timestamp(timestamp ts) {
    static char str[34+11] = {0}; //Total length is 34 characters. We add some reserve (11 bytes -> 1 for each parameter) to suppress GCC7 warnings.
    uint8_t type = ts.typeAndTimezone >> 12;
    int16_t offset = (ts.typeAndTimezone & 0x0800) > 0 ? (ts.typeAndTimezone & 0x0FFF) - (0x1000) : (ts.typeAndTimezone & 0x0FFF);
    int8_t hrso = 0;
    int8_t mino = 0;
    dbg("offset: %d\n", offset);
    if(type == 1 && offset > -2047) { // timestamp is in local time. Convert to UTC.
        hrso = offset/60; // offset in hours
        mino = offset%60; // offset in minutes
    }
    dbg("TypeAndTimezone: 0x%04x\n", ts.typeAndTimezone);
    sprintf(str, "%04d-%02u-%02u %02u:%02u:%02u.%02u%02u%02u+%02d:%02d", ts.year, ts.month, ts.day, ts.hour, ts.minute, ts.second, ts.centiseconds, ts.hundredsOfMicroseconds ,ts.microseconds, hrso, mino);
    return str; 
}

/**
 * \brief UDF timestamp to Unix timestamp conversion function
 *
 * This function fills Unix timestamp structure with its values, adds time offset from UDF
 * timestamp to it and create timestamp.
 *
 * \warning Because Unix timestamp has 'second' as the minimal unit of time, there is precision
 * loss since UDF tracks down to microseconds
 *
 * \param[in] t UDF timestamp
 * \return time_t Unix timestamp structure
 */
time_t timestamp2epoch(timestamp t) {
    struct tm tm;
    tm.tm_wday = 0;   
    tm.tm_yday = 0;   
    tm.tm_isdst = 0;  
    tm.tm_year = t.year - 1900;
    tm.tm_mon = t.month - 1; 
    tm.tm_mday = t.day;
    tm.tm_hour = t.hour;
    tm.tm_min = t.minute;
    tm.tm_sec = t.second;
    float rest = (t.centiseconds * 10000 + t.hundredsOfMicroseconds * 100 + t.microseconds)/1000000.0;
    if(rest > 0.5)
        tm.tm_sec++;
    uint8_t type = t.typeAndTimezone >> 12;
    int16_t offset = (t.typeAndTimezone & 0x0800) > 0 ? (t.typeAndTimezone & 0x0FFF) - (0x1000) : (t.typeAndTimezone & 0x0FFF);
    if(type == 1 && offset > -2047) { // timestamp is in local time. Convert to UTC.
        int8_t hrso = offset/60; // offset in hours
        int8_t mino = offset%60; // offset in minutes
        tm.tm_hour -= hrso;
        tm.tm_min -= mino;
    } else if(type == 2) {
        warn("Time interpretation is not specified.\n");
    }
    return mktime(&tm);
}

/**
 * \brief UDF Timestamp comparison wrapper
 *
 * Timestamps are converted to Unix timestamps and compared with difftime()
 *
 * \param[in] a first timestamp
 * \param[in] b second timestamp
 * \return result of difftime(). Basically result a-b.
 */
double compare_timestamps(timestamp a, timestamp b) {
    double dt = difftime(timestamp2epoch(a), timestamp2epoch(b));
    return dt;
}

/**
 * \brief File information printing function
 *
 * This function wraps file characteristics, file type, permissions, modification time, size and
 * record name and prints it in human readable form.
 *
 * Format is this: HdDPM:darwxd:arwxd:arwx <FILETYPE> <TIMESTAMP> <SIZE> "<NAME>"\n
 * - H -- Hidden
 * - d -- Directory
 * - D -- Deleted
 * - P -- Parent
 * - M -- Metadata
 * - d -- right to delete
 * - r -- right to read
 * - w -- right to write
 * - x -- right to execute
 * - a -- right to change attributes
 * - . -- bit not set
 *
 * \param[in] info file information to print
 * \param[in] depth parameter for prefix, required by depth2str().
 */
void print_file_info(struct fileInfo info, uint32_t depth) {
    msg("%s", depth2str(depth));

    //Print file char
    uint8_t deleted = 0;
    for(int i=0; i<5; i++) {
        switch(info.fileCharacteristics & (1 << i)) {
            case FID_FILE_CHAR_HIDDEN:   msg("H"); break;
            case FID_FILE_CHAR_DIRECTORY:msg("d"); break;
            case FID_FILE_CHAR_DELETED:  msg("D"); deleted = 1; break;
            case FID_FILE_CHAR_PARENT:   msg("P"); break;
            case FID_FILE_CHAR_METADATA: msg("M"); break;
            default:                     msg(".");
        }
    }

    if(deleted == 0) {
        msg(":");

        //Print permissions
        for(int i=14; i>=0; i--) {
            switch(info.permissions & (1 << i)) {
                case FE_PERM_O_EXEC:    msg("x");  break;
                case FE_PERM_O_WRITE:   msg("w");  break;
                case FE_PERM_O_READ:    msg("r");  break;
                case FE_PERM_O_CHATTR:  msg("a");  break;
                case FE_PERM_O_DELETE:  msg("d");  break;
                case FE_PERM_G_EXEC:    msg("x");  break;
                case FE_PERM_G_WRITE:   msg("w");  break;
                case FE_PERM_G_READ:    msg("r");  break;
                case FE_PERM_G_CHATTR:  msg("a");  break;
                case FE_PERM_G_DELETE:  msg("d");  break;
                case FE_PERM_U_EXEC:    msg("x");  break;
                case FE_PERM_U_WRITE:   msg("w");  break;
                case FE_PERM_U_READ:    msg("r");  break;
                case FE_PERM_U_CHATTR:  msg("a");  break;
                case FE_PERM_U_DELETE:  msg("d");  break;

                default:                msg(".");
            }
            if(i == 4 || i == 9 ) {
                msg(":");
            }
        }

        switch(info.fileType) {
            case ICBTAG_FILE_TYPE_DIRECTORY: msg(" DIR    "); break;
            case ICBTAG_FILE_TYPE_REGULAR:   msg(" FILE   "); break;
            case ICBTAG_FILE_TYPE_BLOCK:     msg(" BLOCK  "); break;
            case ICBTAG_FILE_TYPE_CHAR:      msg(" CHAR   "); break;
            case ICBTAG_FILE_TYPE_FIFO:      msg(" FIFO   "); break;
            case ICBTAG_FILE_TYPE_SOCKET:    msg(" SOCKET "); break;
            case ICBTAG_FILE_TYPE_SYMLINK:   msg(" SYMLIN "); break;
            case ICBTAG_FILE_TYPE_STREAMDIR: msg(" STREAM "); break;
            default:                     msg(" UNKNOWN   "); break;
        }

        //Print timestamp
        msg(" %s ", print_timestamp(info.modTime));

        //Print size
        msg(" %8" PRIu64 " ", info.size);

    } else {
        msg("          <Unused FID>                                          ");
    }

    //Print filename
    if(info.filename == NULL) {
        msg(" <ROOT> ");
    } else {
        msg(" \"%s\"", info.filename);
    }

    msg("\n");
}

static void sync_chunk(uint8_t **dev, uint32_t chunk, uint64_t devsize) {
    uint32_t chunksize = CHUNK_SIZE;
    uint64_t rest = devsize % chunksize;
    if(dev[chunk] != NULL) {
#ifndef MEMTRACE
        dbg("Going to sync chunk #%u\n", chunk);
#else
        dbg("Going to sync chunk #%u, ptr: %p\n", chunk, dev[chunk]);
#endif
        if((rest > 0) && (chunk == (devsize / chunksize))) {
            dbg("\tRest used\n");
            msync(dev[chunk], chunksize, MS_SYNC);
        } else {
            dbg("\tChunk size used\n");
            msync(dev[chunk], chunksize, MS_SYNC);
        }
        dbg("\tChunk #%u synced\n", chunk);
    } else {
        dbg("\tChunk #%u is unmapped\n", chunk);
    }
}

void unmap_chunk(udf_media_t *media, uint32_t chunk) {
    uint32_t chunksize = CHUNK_SIZE;
    uint64_t rest = media->devsize % chunksize;
    if (media->mapping[chunk] != NULL) {
        sync_chunk(media->mapping, chunk, media->devsize);
#ifndef MEMTRACE
        dbg("Going to unmap chunk #%u\n", chunk);
#else
        dbg("Going to unmap chunk #%u, ptr: %p\n", chunk, media->mapping[chunk]);
#endif
        if ((rest > 0) && (chunk == (media->devsize / chunksize))) {
            dbg("\tRest used\n");
            munmap(media->mapping[chunk], rest);
        } else {
            dbg("\tChunk size used\n");
            munmap(media->mapping[chunk], chunksize);
        }
        media->mapping[chunk] = NULL;
        dbg("\tChunk #%u unmapped\n", chunk);
    } else {
        dbg("\tChunk #%u is already unmapped\n", chunk);
#ifdef MEMTRACE
        dbg("[MEMTRACE] Chunk #%u is already unmapped\n", chunk);
#endif
    }
}

void map_chunk(udf_media_t* media, uint32_t chunk, char * file, int line) {
    uint32_t chunksize = CHUNK_SIZE;
    uint32_t rest = (uint32_t) (media->devsize % chunksize);
    if (media->mapping[chunk] != NULL) {
        dbg("\tChunk #%u is already mapped.\n", chunk);
        return;
    }
#ifdef MEMTRACE
    dbg("[MEMTRACE] map_chunk source call: %s:%d\n", file, line);
#endif
    dbg("\tSize: 0x%" PRIx64 ", chunk size 0x%x, rest: 0x%x\n", media->devsize, chunksize, rest);

    int prot = PROT_READ;
    // If is there some request for corrections, we need read/write access to the medium
    if(interactive || autofix) {
        prot |= PROT_WRITE;
        dbg("\tRW\n");
    }

    dbg("\tdevsize/chunksize = %" PRIu64 "\n", media->devsize / chunksize);
    if ((rest > 0) && (chunk == (media->devsize / chunksize))) {
        dbg("\tRest used\n");
        media->mapping[chunk] = (uint8_t *)mmap(NULL, rest, prot, MAP_SHARED, media->fd, (uint64_t)(chunk)*chunksize);
    } else {
        dbg("\tChunk size used\n");
        media->mapping[chunk] = (uint8_t *)mmap(NULL, chunksize, prot, MAP_SHARED, media->fd, (uint64_t)(chunk)*chunksize);
    }
    if (media->mapping[chunk] == MAP_FAILED) {
        switch(errno) {
            case EACCES: dbg("EACCES\n"); break;
            case EAGAIN: dbg("EAGAIN\n"); break;
            case EBADF: dbg("EBADF\n"); break;
            case EINVAL: dbg("EINVAL\n"); break;
            case ENFILE: dbg("ENFILE\n"); break;
            case ENODEV: dbg("ENODEV\n"); break;
            case ENOMEM: dbg("ENOMEM\n"); break;
            case EPERM: dbg("EPERM\n"); break;
            case ETXTBSY: dbg("ETXTBSY\n"); break;
            case EOVERFLOW: dbg("EOVERFLOW\n"); break;
            default: dbg("EUnknown\n"); break;
        }

        fatal("\tError mapping: %s.\n", strerror(errno));
        exit(ESTATUS_OPERATIONAL_ERROR);
    }
#ifdef MEMTRACE
    dbg("\tChunk #%u allocated, pointer: %p, offset 0x%" PRIx64 "\n", chunk,
        media->mapping[chunk], (uint64_t)(chunk)*chunksize);
#else
    dbg("\tChunk #%u allocated\n", chunk);
#endif

    // Suppressing unused variables
    (void)file;
    (void)line;
}

void unmap_raw(uint8_t **ptr, uint32_t offset, size_t size) {
    if(*ptr != NULL) {
#ifdef MEMTRACE
        dbg("Going to unmap area, ptr: %p\n", ptr);
#endif
        munmap(*ptr, size);
        *ptr = NULL;
        dbg("\tArea unmapped\n");
    } else {
        dbg("\tArea is already unmapped\n");
    }

    (void)offset;
}

static void map_raw(int fd, uint8_t **ptr, uint64_t offset, size_t size, uint64_t devsize) {
    if(*ptr != NULL) {
        dbg("\tArea is already mapped.\n");
        return;
    }

    dbg("\tSize: 0x%" PRIx64 ", Alloc size 0x%zx\n", devsize, size);

    int prot = PROT_READ;
    // If is there some request for corrections, we need read/write access to the medium
    if(interactive || autofix) {
        prot |= PROT_WRITE;
        dbg("\tRW\n");
    }

    *ptr = (uint8_t *)mmap(NULL, size, prot, MAP_SHARED, fd, offset);
    if(ptr == MAP_FAILED) {
        switch(errno) {
            case EACCES: dbg("EACCES\n"); break;
            case EAGAIN: dbg("EAGAIN\n"); break;
            case EBADF: dbg("EBADF\n"); break;
            case EINVAL: dbg("EINVAL\n"); break;
            case ENFILE: dbg("ENFILE\n"); break;
            case ENODEV: dbg("ENODEV\n"); break;
            case ENOMEM: dbg("ENOMEM\n"); break;
            case EPERM: dbg("EPERM\n"); break;
            case ETXTBSY: dbg("ETXTBSY\n"); break;
            case EOVERFLOW: dbg("EOVERFLOW\n"); break;
            default: dbg("EUnknown\n"); break;
        }

        fatal("\tError mapping: %s.\n", strerror(errno));
        exit(ESTATUS_OPERATIONAL_ERROR);
    }
#ifdef MEMTRACE
    dbg("\tArea allocated, pointer: %p, offset 0x%" PRIx64 "\n", ptr, offset);
#else
    dbg("\tArea allocated\n");
#endif
}

char * dstring_suberror(uint8_t e_code) {
   switch(e_code) {
        case 0:
            return NULL;
        case DSTRING_E_NONZERO_PADDING:
            return "non-zero padding";
        case DSTRING_E_WRONG_LENGTH:
            return "wrong length";
        case DSTRING_E_INVALID_CHARACTERS:
            return "invalid characters present";
        case DSTRING_E_NOT_EMPTY:
            return "string is not empty";
        case DSTRING_E_UNKNOWN_COMP_ID:
            return "unknown Compression ID";
        default: 
            return "unknown dstring error";
   } 
}

uint8_t dstring_error(char * string_name, uint8_t e_code) {
    if(e_code > 0) {
        msg("Dstring %s has following errors:\n", string_name);
        for(int i=0; i<8; ++i) {
            if(e_code & 1<<i) {
                msg("\t- %s\n", dstring_suberror(e_code & 1<<i));
            }
        }

        return ESTATUS_UNCORRECTED_ERRORS;
    }
    return ESTATUS_OK;
}

/**
 * \brief Function for detection of errors in dstrings
 *
 * This function detects violation against UDF 2.1.1 in dstrings
 * These violations are:
 *  1. Non-zero padding
 *  2. Not allowed characters for 16 bit dchars (0xFFFE and 0xFEFF)
 *  3. Unknown compression ID
 *  4. Emptiness of string if ID is 0 or length is 0
 *  5. String length mismatch
 *
 * \param[in] in dstring for check
 * \param[in] field_size size of dstring field
 * \return sum of DSTRING_E codes 
 */
uint8_t check_dstring(dstring *in, size_t field_size) {
    uint8_t compID = in[0];
    uint8_t length = in[field_size-1];
    uint8_t stepping = 0xFF;
    uint8_t empty_flag = 0;
    uint8_t e_code = 0;
    uint8_t no_length = 0;

    dbg("compID: %u, length: %u\n", compID, length);
    switch(compID) {
        case 8: 
            stepping = 1;
            break;
        case 16: 
            stepping = 2;
            break;
        case 0:
            stepping = 1;
            empty_flag = 1;
            break;
        case 254:
            stepping = 1;
            no_length = 1;
            break;
        case 255:
            stepping = 2;
            no_length = 1;
            break;
        default:
            err("Unknown dstring compression ID.\n");
            return DSTRING_E_UNKNOWN_COMP_ID;
    }

    if(empty_flag || (length == 0 && no_length == 0)) {
        // Check for emptiness
        dbg("Empty check\n");
        for(int i = 0; i < (int)field_size; i += stepping) {
            if(in[i] != 0) {
                err("Dstring is not empty.\n");
                e_code |= DSTRING_E_NOT_EMPTY;
            }
        }
    } else {
        // Leave first byte, it contains compression code.
        // Last bit contains length OR characters if compID is 254 or 255.
        // Stepping +1 if 8bit or +2 if 16bit character length
        
        if(no_length == 0) {
            dbg("Length and padding check\n");
            // Check for length and zero padding.
            uint8_t char_count = 0;
            uint8_t eol_flag = 0xFF;
            for(int i = 1; i < (int)(field_size-1); i += stepping) {
                // We need to check if character is 0.
                // For 8bit: we check character twice to keep code simple.
                // For 16bit: we check character i and i+1
                // 
                // If hole (NULL character) detected, eol_flag is set.
                // If eol is set and characted is detected, it is violation of UDF 2.1.1
                if(in[i] != 0 || in[i+stepping-1] != 0) {
                    if(eol_flag < 0xFF) {
                        err("Dstring has non-zero padding\n");
                        e_code |= DSTRING_E_NONZERO_PADDING;
                    } else {
                        ++char_count;
                    }
                } else {
                    if(eol_flag == 0xFF) {
                        eol_flag = i;
                    }
                }
            }

            dbg("EOL check\n");
            // eol_flag contains first NULL position. 
            if(((length) != (eol_flag)) && eol_flag != 0xFF ) {
                err("Dstring has mismatch between actual and declared length\n");
                dbg("eol_flag: %u\n", eol_flag);
                e_code |= DSTRING_E_WRONG_LENGTH;
            }
        }

        // Check for valid characters. Only for 16 bit makes sense.
        // All Unicode 1.1 characters are valid. Only endianness codes are invalid (0xFFFE and 0xFEFF)
        if(stepping == 2) {
            dbg("Invalid chars check\n");
            for(int i = 1; i < (int)(field_size-1); i += stepping) {
                if((in[i] == 0xFF && in[i+1] == 0xFE) || (in[i] == 0xFE && in[i+1] == 0xFF)) {
                    err("Dstring contains invalid characters\n");
                    e_code |= DSTRING_E_INVALID_CHARACTERS;
                }
            } 
        }
    }
    return e_code;
}

/**
 * \brief UDF VRS detection function
 *
 * This function tries to find VRS at sector 16.
 * It also makes the first attempt to guess sectorsize.
 *
 * \param[in] media             Information regarding medium & access to it
 * \param[in] force_sectorsize  if -b param is used, this flag should be set
 *                                and sectorsize should be used automatically.
 *
 * \return 0 -- UDF successfully detected, sectorsize candidate found
 * \return -1 -- found BOOT2 or CDW02. Unsupported for now
 * \return 1 -- UDF not detected 
 */
int is_udf(udf_media_t* media, int force_sectorsize, struct filesystemStats *stats) {
    const struct volStructDesc *vsd = NULL;
    const struct beginningExtendedAreaDesc *bea = NULL;
    const struct volStructDesc *nsr = NULL;
    const struct terminatingExtendedAreaDesc *tea = NULL;
    int ssize = BLOCK_SIZE;  // No point in attempting smaller sizes here
    int notFound = 0;
    int foundBEA = 0;
    uint32_t chunk = 0;
    uint32_t chunksize = CHUNK_SIZE;

    for(int it=0; it<2; it++, ssize *= 2) {
        if(force_sectorsize) {
            ssize = media->sectorsize;
            it = INT_MAX - 1; //End after this iteration
            dbg("Forced sectorsize\n");
        }

        dbg("Try sectorsize %d\n", MIN(ssize, BLOCK_SIZE));

        for(int i = 0; i<6; i++) {
            uint32_t byte_offset = 16 * BLOCK_SIZE + i*MAX(ssize, BLOCK_SIZE);
            chunk =  byte_offset / chunksize;
            map_chunk(media, chunk, __FILE__, __LINE__);
            dbg("try #%d at address 0x%x, chunk %u, chunk address: 0x%x\n", i, byte_offset, chunk,
                byte_offset %chunksize);
#ifdef MEMTRACE
            dbg("Chunk pointer: %p\n", media->mapping[chunk]);
#endif
            vsd = (const struct volStructDesc *)(media->mapping[chunk] + (byte_offset % chunksize));
            dbg("vsd: type:%u, id:%.5s, v:%u\n", vsd->structType, vsd->stdIdent, vsd->structVersion);

            if(!strncmp((const char *)vsd->stdIdent, VSD_STD_ID_BEA01, 5)) {
                //It's Extended area descriptor, so it might be UDF, check next sector
                bea = (const struct beginningExtendedAreaDesc *) vsd; // store it for later
                foundBEA = 1; 
            } else if(!strncmp((const char *)vsd->stdIdent, VSD_STD_ID_BOOT2, 5)) {
                if (!foundBEA) {
                    err("BOOT2 found outside of VRS extended area.\n");
                    unmap_chunk(media, chunk);
                    return -1;
                }
                // Don't fail BOOT2 otherwise; UDF (through at least 2.60) has never
                // prohibited or specified it.
                // It might be present *within* BEA01 or NSR blocks in order to
                // band-aid broken Windows VRS processing when sectors are 4096 bytes.
                // See https://lkml.org/lkml/2019/7/9/596
            } else if(!strncmp((const char *)vsd->stdIdent, VSD_STD_ID_CD001, 5)) {
                //CD001 means there is ISO9660, we try search for UDF at sector 18
            } else if(!strncmp((const char *)vsd->stdIdent, VSD_STD_ID_CDW02, 5)) {
                err("CDW02 found, unsuported for now.\n");
                unmap_chunk(media, chunk);
                return -1;
            } else if(!strncmp((const char *)vsd->stdIdent, VSD_STD_ID_NSR02, 5)) {
                nsr = vsd;
            } else if(!strncmp((const char *)vsd->stdIdent, VSD_STD_ID_NSR03, 5)) {
                nsr = vsd;
            } else if(!strncmp((const char *)vsd->stdIdent, VSD_STD_ID_TEA01, 5)) {
                //We found TEA01, so we can end recognition sequence
                tea = (const struct terminatingExtendedAreaDesc *) vsd;
                break;
            } else if(vsd->stdIdent[0] == '\0') {
                if(foundBEA) {
                    continue;
                }
                notFound = 1;
                break;
            } else {
                err("Unknown identifier: %.5s. Exiting\n", vsd->stdIdent);
                notFound = 1;
                break;
            }  
        }

        if(notFound) {
            notFound = 0;
            continue;
        }

        if (bea)
            dbg("bea: type:%u, id:%.5s, v:%u\n", bea->structType, bea->stdIdent, bea->structVersion);
        else
            err("bea: not found\n");

        if (nsr) {
            dbg("nsr: type:%u, id:%.5s, v:%u\n", nsr->structType, nsr->stdIdent, nsr->structVersion);
            update_min_udf_revision(stats, nsr->stdIdent[4] == '3' ? 0x0200 : 0x0100);
        } else
            err("nsr: not found\n");

        if (tea)
            dbg("tea: type:%u, id:%.5s, v:%u\n", tea->structType, tea->stdIdent, tea->structVersion);
        else
            err("tea: not found\n");

        // Sector size determination is conclusive only when sectors are larger
        // than Volume Structure Descriptors
        if (ssize > BLOCK_SIZE)
            media->sectorsize = ssize;
        unmap_chunk(media, chunk);
        return 0;
    }

    err("Giving up VRS, maybe unclosed or bridged disc.\n");
    unmap_chunk(media, chunk);
    return 1;
}

/**
 * \brief Locate AVDP on device and store it
 *
 * This function checks for AVDP at a specified well-known position.
 * If found, the AVDP is stored to the appropriate place in the udf_disc structure.
 *
 * It also determine sector size, since AVDP have fixed position. 
 *
 * \param[in] media   Information regarding medium & access to it
 * \param[in] type    selector of AVDP - first or second
 * \param[in] *stats  statistics of file system
 *
 * \return  0 everything is ok
 * \return 255 Only for Third AVDP: it is not AVDP. Abort.
 * \return sum of E_CRC, E_CHECKSUM, E_WRONGDESC, E_POSITION, E_EXTLEN  
 */
int get_avdp(udf_media_t *media, avdp_type_e type, int force_sectorsize,
             struct filesystemStats *stats) {
    int64_t position = 0;
    tag desc_tag;
    int ssize = 512;
    int status = 0;
    uint32_t chunksize = CHUNK_SIZE;
    uint32_t chunk = 0;
    uint32_t offset = 0;

    for(int it = 0; it < 5; it++, ssize *= 2) { 

        //Check if sectorsize is already found
        if(force_sectorsize) {
            ssize = media->sectorsize;
            it = INT_MAX-1; //break after this round
        }
        dbg("Trying sectorsize %d\n", ssize);

        //Reset status for new round
        status = 0;

        if(type == 0) {
            position = ssize*256; //First AVDP is on LSN=256
        } else if(type == 1) {
            position = media->devsize - ssize; // Second AVDP is on last LSN
        } else if(type == 2) {
            position = media->devsize - ssize - 256*ssize; // Third AVDP can be at last LSN-256
        } else {
            position = ssize*512; //Unclosed disc have AVDP at sector 512
            type = 0; //Save it to FIRST_AVDP position
        }

        dbg("DevSize: %" PRIu64 "\n", media->devsize);
        dbg("Current position: %" PRIx64 "\n", position);
        chunk = position/chunksize;
        offset = position%chunksize;
        dbg("Chunk: %u, offset: 0x%x\n", chunk, offset);
        map_chunk(media, chunk, __FILE__, __LINE__);

        if (media->disc.udf_anchor[type] == NULL) {
            media->disc.udf_anchor[type] = malloc(sizeof(struct anchorVolDescPtr)); // Prepare memory for AVDP
        }

#ifdef MEMTRACE
        dbg("AVDP chunk ptr: %p\n", media->mapping[chunk] + offset);
#endif
        desc_tag = *(tag *)(media->mapping[chunk] + offset);
        dbg("Tag allocated\n");

        if(!checksum(desc_tag)) {
            status |= E_CHECKSUM;
            unmap_chunk(media, chunk);
            if(type == THIRD_AVDP) {
                return -1;
            } 
            continue;
        }
        if(le16_to_cpu(desc_tag.tagIdent) != TAG_IDENT_AVDP) {
            status |= E_WRONGDESC;
            unmap_chunk(media, chunk);
            if(type == THIRD_AVDP) {
                return -1;
            } 
            continue;
        }
        dbg("Tag Serial Num: %u\n", desc_tag.tagSerialNum);
        if(stats->AVDPSerialNum == 0xFFFF) { // Default state -> save first found 
            stats->AVDPSerialNum = desc_tag.tagSerialNum;
        } else if(stats->AVDPSerialNum != desc_tag.tagSerialNum) { //AVDP serial number differs, no recovery support. UDF 2.1.6
            stats->AVDPSerialNum = 0; //No recovery support
        }

        memcpy(media->disc.udf_anchor[type], media->mapping[chunk]+offset, sizeof(struct anchorVolDescPtr));

        if (crc(media->disc.udf_anchor[type], sizeof(struct anchorVolDescPtr))) {
            // Some implementations mistakenly use a short descCRCLength
            // that doesn't cover the large 'reserved' region of the AVDP.
            // This does not bother Windows or Linux, don't let it bother us.
            uint16_t shortenedDescSize = offsetof(struct anchorVolDescPtr, reserved);
            if (   (desc_tag.descCRCLength == (shortenedDescSize - sizeof(tag)))
                && (crc(media->disc.udf_anchor[type], shortenedDescSize) == 0)) {
                warn("AVDP descCRCLength is non-compliant\n");
            }
            else {
                status |= E_CRC;
                unmap_chunk(media, chunk);
                continue;
            }
        }

        if(check_position(desc_tag, position/ssize)) {
            status |= E_POSITION;
            unmap_chunk(media, chunk);
            continue;
        }

        dbg("AVDP[%d]: Main Ext Len: %u, Reserve Ext Len: %u\n", type,
            media->disc.udf_anchor[type]->mainVolDescSeqExt.extLength,
            media->disc.udf_anchor[type]->reserveVolDescSeqExt.extLength);
        dbg("AVDP[%d]: Main Ext Pos: 0x%08x, Reserve Ext Pos: 0x%08x\n", type,
            media->disc.udf_anchor[type]->mainVolDescSeqExt.extLocation,
            media->disc.udf_anchor[type]->reserveVolDescSeqExt.extLocation);
        if (   (media->disc.udf_anchor[type]->mainVolDescSeqExt.extLength < (uint32_t)(16*ssize))
            || (media->disc.udf_anchor[type]->reserveVolDescSeqExt.extLength < (uint32_t)(16*ssize))) {
            status |= E_EXTLEN;
        }

        msg("AVDP[%d] successfully loaded.\n", type);
        media->sectorsize = ssize;

        if(status & E_CHECKSUM) {
            err("Checksum failure at AVDP[%d]\n", type);
        }
        if(status & E_WRONGDESC) {
            err("AVDP not found at 0x%" PRIx64 "\n", position);
        }
        if(status & E_CRC) {
            err("CRC error at AVDP[%d]\n", type);
        }
        if(status & E_POSITION) {
            err("Position mismatch at AVDP[%d]\n", type);
        }
        if(status & E_EXTLEN) {
            err("Main or Reserve Extent Length at AVDP[%d] is less than 16 sectors\n", type);
        }  
        unmap_chunk(media, chunk);
        return status;
    }
    unmap_chunk(media, chunk);
    return status;
}


/**
 * \brief Loads Volume Descriptor Sequence (VDS) and stores it at struct udf_disc
 *
 * \param[in] media   Information regarding medium & access to it
 * \param[in] vds     MAIN_VDS or RESERVE_VDS selector
 * \param[out] *seq   structure capturing actual order of descriptors in VDS for recovery
 *
 * \return 0 everything ok
 *         -3 found unknown tag
 *         -4 descriptor is already set
 */
int get_vds(udf_media_t *media, avdp_type_e avdp, vds_type_e vds, vds_sequence_t *seq) {
    uint8_t *position;
    uint8_t *raw = NULL;
    int8_t counter = 0;
    tag descTag;
    uint64_t location = 0;
    uint32_t chunksize = CHUNK_SIZE;
    uint32_t chunk = 0;
    uint32_t offset = 0;
    uint32_t descLen;

    // Go to first address of VDS
    switch(vds) {
        case MAIN_VDS:
            location =   media->sectorsize
                       * ((uint64_t)(media->disc.udf_anchor[avdp]->mainVolDescSeqExt.extLocation));
            dbg("VDS location: 0x%x\n", media->disc.udf_anchor[avdp]->mainVolDescSeqExt.extLocation);
            break;

        case RESERVE_VDS:
            location =   media->sectorsize
                       * ((uint64_t)(media->disc.udf_anchor[avdp]->reserveVolDescSeqExt.extLocation));
            dbg("VDS location: 0x%x\n", media->disc.udf_anchor[avdp]->reserveVolDescSeqExt.extLocation);
            break;
    }
    chunk = location/chunksize;
    offset = (uint32_t)(location % (uint64_t)chunksize);
    map_chunk(media, chunk, __FILE__, __LINE__);
    position = media->mapping[chunk]+offset;
    dbg("VDS Location: 0x%" PRIx64 ", chunk: %u, offset: 0x%x\n", location, chunk, offset);

    // Go thru descriptors until TagIdent is 0 or amount is too big to be real
    while(counter < VDS_STRUCT_AMOUNT) {

        // Read tag
        memcpy(&descTag, position, sizeof(descTag));

        dbg("Tag ID: %u\n", descTag.tagIdent);

        if(vds == MAIN_VDS) {
            seq->main[counter].tagIdent = descTag.tagIdent;
            seq->main[counter].tagLocation = (location) / media->sectorsize;
        } else {
            seq->reserve[counter].tagIdent = descTag.tagIdent;
            seq->reserve[counter].tagLocation = (location) / media->sectorsize;
        }

        counter++;
        dbg("Tag stored\n");

        // What kind of descriptor is that?
        switch(le16_to_cpu(descTag.tagIdent)) {
            case TAG_IDENT_PVD:
                descLen = sizeof(struct primaryVolDesc);
                if (media->disc.udf_pvd[vds] != 0) {
                    err("Structure PVD is already set. Probably error in tag or media\n");
                    unmap_chunk(media, chunk);
                    return -4;
                }
                media->disc.udf_pvd[vds] = malloc(descLen); // Prepare memory
                memcpy(media->disc.udf_pvd[vds], position, descLen);
                dbg("VolNum: %u\n",  media->disc.udf_pvd[vds]->volDescSeqNum);
                dbg("pVolNum: %u\n", media->disc.udf_pvd[vds]->primaryVolDescNum);
                dbg("seqNum: %u\n",  media->disc.udf_pvd[vds]->volSeqNum);
                dbg("predLoc: %u\n", media->disc.udf_pvd[vds]->predecessorVolDescSeqLocation);
                break;

            case TAG_IDENT_IUVD:
                descLen = sizeof(struct impUseVolDesc);
                if (media->disc.udf_iuvd[vds] != 0) {
                    err("Structure IUVD is already set. Probably error in tag or media\n");
                    unmap_chunk(media, chunk);
                    return -4;
                }
                dbg("Store IUVD\n");
                media->disc.udf_iuvd[vds] = malloc(descLen); // Prepare memory
#ifdef MEMTRACE
                dbg("Malloc ptr: %p\n", media->disc.udf_iuvd[vds]);
#endif
                memcpy(media->disc.udf_iuvd[vds], position, descLen);
                dbg("Stored\n"); 
                break;

            case TAG_IDENT_PD:
                descLen = sizeof(struct partitionDesc);
                if (media->disc.udf_pd[vds] != 0) {
                    err("Structure PD is already set. Probably error in tag or media\n");
                    unmap_chunk(media, chunk);
                    return -4;
                }
                media->disc.udf_pd[vds] = malloc(descLen); // Prepare memory
                memcpy(media->disc.udf_pd[vds], position, descLen);
                break;

            case TAG_IDENT_LVD:
                if (media->disc.udf_lvd[vds] != 0) {
                    err("Structure LVD is already set. Probably error in tag or media\n");
                    unmap_chunk(media, chunk);
                    return -4;
                }
                dbg("LVD size: 0x%zx\n", sizeof(struct logicalVolDesc));

                struct logicalVolDesc *lvd;
                lvd = (struct logicalVolDesc *)(position);

                descLen = sizeof(struct logicalVolDesc) + le32_to_cpu(lvd->mapTableLength);
                media->disc.udf_lvd[vds] = malloc(descLen); // Prepare memory

                map_raw(media->fd, &raw, (uint64_t)(chunk)*CHUNK_SIZE, descLen + offset, media->devsize);
                memcpy(media->disc.udf_lvd[vds], raw+offset, descLen);
                unmap_raw(&raw, (uint64_t)(chunk)*CHUNK_SIZE, descLen + offset);

                dbg("NumOfPartitionMaps: %u\n", media->disc.udf_lvd[vds]->numPartitionMaps);
                dbg("MapTableLength: %u\n",     media->disc.udf_lvd[vds]->mapTableLength);
                for(int i=0; i<(int)(le32_to_cpu(lvd->mapTableLength)); i++) {
                    note("[0x%02x] ", media->disc.udf_lvd[vds]->partitionMaps[i]);
                }
                note("\n");
                break;

            case TAG_IDENT_USD:
                if (media->disc.udf_usd[vds] != 0) {
                    err("Structure USD is already set. Probably error in tag or media\n");
                    unmap_chunk(media, chunk);
                    return -4;
                }

                struct unallocSpaceDesc *usd;
                usd = (struct unallocSpaceDesc *)(position);
                dbg("VolDescNum: %u\n", usd->volDescSeqNum);
                dbg("NumAllocDesc: %u\n", usd->numAllocDescs);

                descLen =   sizeof(struct unallocSpaceDesc)
                          + le32_to_cpu(usd->numAllocDescs) * sizeof(extent_ad);
                media->disc.udf_usd[vds] = malloc(descLen); // Prepare memory

                map_raw(media->fd, &raw, (uint64_t)(chunk)*CHUNK_SIZE, descLen + offset, media->devsize);
                memcpy(media->disc.udf_usd[vds], raw+offset, descLen);
                unmap_raw(&raw, (uint64_t)(chunk)*CHUNK_SIZE, descLen + offset);
                break;

            case TAG_IDENT_TD:
                if (media->disc.udf_td[vds] != 0) {
                    err("Structure TD is already set. Probably error in tag or media\n");
                    unmap_chunk(media, chunk);
                    return -4;
                }
                descLen = sizeof(struct terminatingDesc);
                media->disc.udf_td[vds] = malloc(descLen); // Prepare memory
                memcpy(media->disc.udf_td[vds], position, descLen);
                // Found terminator, ending.
                unmap_chunk(media, chunk);
                return 0;

            case 0:
                // Found end of VDS, ending.
                unmap_chunk(media, chunk);
                return 0;

            default:
                // Unknown TAG
                fatal("Unknown TAG found at %p. Ending.\n", position);
                unmap_chunk(media, chunk);
                return -3;
        }

        dbg("Unmap old chunk...\n");
        unmap_chunk(media, chunk);
        dbg("Unmapped\n");
        location = location + media->sectorsize * ((descLen + media->sectorsize - 1) / media->sectorsize);
        chunk = location/chunksize;
        offset = location%chunksize;
        dbg("New VDS Location: 0x%" PRIx64 ", chunk: %u, offset: 0x%x\n", location, chunk, offset);
        map_chunk(media, chunk, __FILE__, __LINE__);
        position = media->mapping[chunk] + offset;
    }
    //unmap_chunk(dev, chunk);
    return 0;
}

/**
 * \brief Selects **MAIN_VDS** or **RESERVE_VDS** for required descriptor based on errors
 *
 * If some function needs some descriptor from VDS, it requires a check that the descriptor is
 * structurally correct.
 * This is already checked and stored in seq->main[vds].error and seq->reserve[vds].error.
 * This function searches thru this sequence based on tagIdent and looks at errors when found.
 *
 * \param[in] *seq descriptor sequence
 * \param[in] tagIdent identifier to find
 * \return MAIN_VDS or RESERVE_VDS if correct descriptor found
 * \return -1 if no correct descriptor found or both are broken.
 */
int get_correct(vds_sequence_t *seq, uint16_t tagIdent) {
    for(int i=0; i<VDS_STRUCT_AMOUNT; i++) {
        if(seq->main[i].tagIdent == tagIdent && (seq->main[i].error & (E_CRC | E_CHECKSUM | E_WRONGDESC)) == 0) {
            return MAIN_VDS; 
        } else if(seq->reserve[i].tagIdent == tagIdent && (seq->reserve[i].error & (E_CRC | E_CHECKSUM | E_WRONGDESC)) == 0) {
            return RESERVE_VDS;
        }
    }
    return -1; 
}

/**
 * \brief Loads Logical Volume Integrity Descriptor (LVID) and stores it at struct udf_disc
 *
 * Loads LVID descriptor to disc structure. Beside that, it stores selected params in stats structure for
 * easier access.
 *
 * \param[in]  media                  Information regarding medium & access to it
 * \param[out] *info                  values from recorded LVID
 * \param[in] *seq                    descriptor sequence
 * \return ESTATUS_OK                 everything ok
 * \return ESTATUS_UNCORRECTED_ERRORS structure is already set or no correct LVID found
 */
int get_lvid(udf_media_t *media, integrity_info_t *info, vds_sequence_t *seq ) {
    uint32_t chunksize = CHUNK_SIZE;
    uint32_t chunk = 0;
    uint32_t offset = 0;
    uint64_t position = 0;

    if (media->disc.udf_lvid != 0) {
        err("Structure LVID is already set. Probably error in tag or media\n");
        return ESTATUS_UNCORRECTED_ERRORS;
    }
    int vds = -1;
    if((vds=get_correct(seq, TAG_IDENT_LVD)) < 0) {
        err("No correct LVD found. Aborting.\n");
        return ESTATUS_UNCORRECTED_ERRORS;
    }

    uint32_t loc = media->disc.udf_lvd[vds]->integritySeqExt.extLocation;
    uint32_t len = media->disc.udf_lvd[vds]->integritySeqExt.extLength;
    dbg("LVID: loc: %u, len: %u\n", loc, len);

    position = loc * (uint64_t)media->sectorsize;
    chunk  = (uint32_t)(position / chunksize);
    offset = (uint32_t)(position % chunksize);
    map_chunk(media, chunk, __FILE__, __LINE__);

    struct logicalVolIntegrityDesc *lvid;
    lvid = (struct logicalVolIntegrityDesc *)(media->mapping[chunk] + offset);

    media->disc.udf_lvid = malloc(len);
    memcpy(media->disc.udf_lvid, media->mapping[chunk]+offset, len);

    if (lvid->descTag.tagIdent != TAG_IDENT_LVID) {
        err("LVID not found\n");
        seq->lvid.error |= E_WRONGDESC;
        unmap_chunk(media, chunk);
        // Attempt to continue, maybe we can rebuild it
        return ESTATUS_OK;
    }
    else {
        if (!checksum(lvid->descTag)) {
            err("LVID checksum error. Continue with caution.\n");
            seq->lvid.error |= E_CHECKSUM;
        }
        if (crc(lvid, lvid->descTag.descCRCLength + sizeof(tag))) {
            err("LVID CRC error. Continue with caution.\n");
            seq->lvid.error |= E_CRC;
        }
    }

    dbg("LVID: lenOfImpUse: %u\n",     media->disc.udf_lvid->lengthOfImpUse);
    dbg("LVID: numOfPartitions: %u\n", media->disc.udf_lvid->numOfPartitions);

    struct impUseLVID *impUse =
        (struct impUseLVID *)(  (uint8_t *)(media->disc.udf_lvid)
                              + sizeof(struct logicalVolIntegrityDesc)
                              + 8 * media->disc.udf_lvid->numOfPartitions); // Because of ECMA 167r3, 3/24, fig 22
    struct logicalVolHeaderDesc *lvhd =
        (struct logicalVolHeaderDesc *)(media->disc.udf_lvid->logicalVolContentsUse);
    info->nextUID = lvhd->uniqueID;

    info->recordedTime = lvid->recordingDateAndTime;

    dbg("LVID: number of files: %u\n", impUse->numOfFiles);
    dbg("LVID: number of dirs:  %u\n", impUse->numOfDirs);
    dbg("LVID: UDF rev: min read:  %04x\n", impUse->minUDFReadRev);
    dbg("               min write: %04x\n", impUse->minUDFWriteRev);
    dbg("               max write: %04x\n", impUse->maxUDFWriteRev);
    dbg("Next Unique ID: %" PRIu64 "\n", info->nextUID);
    dbg("LVID recording timestamp: %s\n", print_timestamp(info->recordedTime));

    info->numFiles = impUse->numOfFiles;
    info->numDirs = impUse->numOfDirs;

    info->minUDFReadRev = impUse->minUDFReadRev;
    info->minUDFWriteRev = impUse->minUDFWriteRev;
    info->maxUDFWriteRev = impUse->maxUDFWriteRev;

    dbg("Logical Volume Contents Use\n");
    for(int i=0; i<32; ) {
        for(int j=0; j<8; j++, i++) {
            note("%02x ", media->disc.udf_lvid->logicalVolContentsUse[i]);
        }
        note("\n");
    }
    dbg("Free Space Table\n");
    const uint32_t *freeSpaceTable = (const uint32_t *) media->disc.udf_lvid->data;
    const uint32_t *sizeTable      = freeSpaceTable + media->disc.udf_lvid->numOfPartitions;
    for(uint32_t i=0; i < media->disc.udf_lvid->numOfPartitions; i++) {
        note("0x%08x, %u\n", freeSpaceTable[i], freeSpaceTable[i]);
    }

    info->freeSpaceBlocks    = freeSpaceTable[0];
    info->partitionNumBlocks = sizeTable[0];

    dbg("Size Table\n");
    for(uint32_t i=0; i < media->disc.udf_lvid->numOfPartitions; i++) {
        note("0x%08x, %u\n", sizeTable[i], sizeTable[i]);
    }

    if (media->disc.udf_lvid->nextIntegrityExt.extLength > 0) {
        dbg("Next integrity extent found.\n");
    } else {
        dbg("No other integrity extents are here.\n");
    }

    unmap_chunk(media, chunk);
    return ESTATUS_OK;
}

/**
 * \brief Checks Logical Block Size stored in LVD against autodetected or declared size.
 *
 * Compare LVD->LogicalBlockSize with detected or declared block size. If they match, fsck can continue.
 * Otherwise it stops with fatal error, because medium is badly created and therefore unfixable.
 *
 * Return can be sum of its parts.
 *
 * \param[in] media            Information regarding medium & access to it
 * \param[in] force_blocksize  1 == user defined sector size
 * \param[in] *seq             descriptor sequence
 *
 * \return ESTATUS_OK                 blocksize matches
 * \return ESTATUS_UNCORRECTED_ERRORS blocksize differs from detected one
 * \return ESTATUS_USAGE              blocksize differs from declared one
 */
int check_blocksize(udf_media_t *media, int force_sectorsize, vds_sequence_t *seq) {

    int vds = -1;
    if((vds=get_correct(seq, TAG_IDENT_LVD)) < 0) {
        err("No correct LVD found. Aborting.\n");
        return ESTATUS_UNCORRECTED_ERRORS;
    }

    int lvd_blocksize = media->disc.udf_lvd[vds]->logicalBlockSize;
    
    if (lvd_blocksize != media->sectorsize) {
        if(force_sectorsize) {
            err("User defined block size does not correspond to medium. Aborting.\n");
            return ESTATUS_USAGE | ESTATUS_UNCORRECTED_ERRORS;
        }

        err("Detected block size does not correspond to medium. Probably badly created UDF. Aborting.\n");
        return ESTATUS_UNCORRECTED_ERRORS;
    }
   
    dbg("Blocksize matches.\n"); 

    return ESTATUS_OK;
}
/**
 * \brief Select various volume identifiers and store them at stats structure
 * 
 * At this moment it selects PVD->volSetIdent and FSD->logicalVolIdent
 *
 * \param[in] *disc disc structure
 * \param[out] *stats file system status structure
 * \param[in] *seq VDS sequence
 *
 * \return 0 -- everything OK
 * \return 4 -- no correct PVD found.
 */
int get_volume_identifier(struct udf_disc *disc, struct filesystemStats *stats, vds_sequence_t *seq ) {
    int vds = -1;
    if((vds=get_correct(seq, TAG_IDENT_PVD)) < 0) {
        err("No correct PVD found. Aborting.\n");
        return 4;
    }
    char *namebuf = calloc(1,128*2);
    memset(namebuf, 0, 128*2);
    decode_string(NULL, disc->udf_pvd[vds]->volSetIdent, namebuf, 128, 128*2);

    for(int i=0; i<16; i++) {
        if(isxdigit(namebuf[i])) {
            continue; 
        } else {
            warn("Volume Set Identifier Unique Identifier is not compliant.\n");
            //append_error(seq, TAG_IDENT_PVD, MAIN_VDS, E_UUID);
            //append_error(seq, TAG_IDENT_PVD, RESERVE_VDS, E_UUID);
            //TODO create fix somewhere. Use this gen_uuid_from_vol_set_ident() for generating new UUID.
            break;
        }
    }

    stats->volumeSetIdent = namebuf;
//  stats->partitionIdent is now set in get_fsd()
    return 0;
}

/**
 * \brief Marks used blocks in actual bitmap
 *
 * This function marks or unmarks specified areas of the block bitmap at stats->actPartitionBitmap
 * If medium is consistent, this bitmap should be same as the recorded one (stats->expPartitionBitmap)
 *
 * \param[in,out] *stats file system status structure
 * \param[in] lbn starting logical block of area
 * \param[in] size length of marked area
 * \param[in] MARK_BLOCK or UNMARK_BLOCK switch
 *
 * \return 0 everything is OK or size is 0 (nothing to mark)
 * \return -1 marking failed (actParititonBitmap is uninitialized)
 */ 
uint8_t markUsedBlock(struct filesystemStats *stats, uint32_t lbn, uint32_t size, uint8_t mark) {
    if ((lbn + size) <= stats->found.partitionNumBlocks) {
        uint32_t byte = 0;
        uint8_t bit = 0;

        dbg("Marked LBN %u with size %u\n", lbn, size);
        if(size == 0) {
            dbg("Size is 0, return.\n");
            return 0;
        }
        int i = 0;
        do {
            byte = lbn/8;
            bit = lbn%8;
            if(mark) { // write 0
                if(stats->actPartitionBitmap[byte] & (1<<bit)) {
                    stats->actPartitionBitmap[byte] &= ~(1<<bit);
                } else {
                    warn("[%u:%u]Error marking block as used. It is already marked.\n", byte, bit);
                }
            } else { // write 1
                if(stats->actPartitionBitmap[byte] & (1<<bit)) {
                    warn("[%u:%u]Error marking block as unused. It is already unmarked.\n", byte, bit);
                } else {
                    stats->actPartitionBitmap[byte] |= 1<<bit;
                }
            }
            lbn++;
            i++;
        } while(i < (int)size);
        dbg("Last LBN: %u, Byte: %u, Bit: %u\n", lbn, byte, bit);
        dbg("Real size: %d\n", i);

#if 0   // For debug purposes only
        note("\n ACT \t EXP\n");
        uint32_t shift = 0;
        for(int i=0+shift, k=0+shift; i<stats->partitionNumBlocks/8 && i < 100+shift; ) {
            for(int j=0; j<16; j++, i++) {
                note("%02x ", stats->actPartitionBitmap[i]);
            }
            note("| "); 
            for(int j=0; j<16; j++, k++) {
                note("%02x ", stats->expPartitionBitmap[k]);
            }
            note("\n");
        }
        note("\n");
        shift = 4400;
        for(int i=0+shift, k=0+shift; i<stats->partitionNumBlocks/8 && i < 100+shift; ) {
            for(int j=0; j<16; j++, i++) {
                note("%02x ", stats->actPartitionBitmap[i]);
            }
            note("| "); 
            for(int j=0; j<16; j++, k++) {
                note("%02x ", stats->expPartitionBitmap[k]);
            }
            note("\n");
        }
        note("\n");
#endif
    } else {
        err("MARKING USED BLOCK TO BITMAP FAILED\n");
        return -1;
    }
    return 0;
}

/**
 * \brief Loads File Set Descriptor and stores it at struct udf_disc
 *
 *  Also checks for dstring defects.
 *
 * \param[in]  media     Information regarding medium & access to it
 * \param[in] *stats     file system status
 * \param[in] *seq       VDS sequence
 *
 * \return 0 everything ok
 * \return 4 no correct PD or LVD found
 * \return 8 error during FSD identification
 */
uint8_t get_fsd(udf_media_t *media,
                struct filesystemStats * stats, vds_sequence_t *seq) {
    long_ad *lap;
    int vds = -1;
    uint32_t offset = 0, chunk = 0;
    uint32_t chunksize = CHUNK_SIZE;
    uint64_t position = 0;

    if((vds=get_correct(seq, TAG_IDENT_PD)) < 0) {
        err("No correct PD found. Aborting.\n");
        return ESTATUS_UNCORRECTED_ERRORS;
    }
    dbg("PD partNum: %u\n", media->disc.udf_pd[vds]->partitionNumber);
    uint32_t lbnlsn = 0;
    lbnlsn = media->disc.udf_pd[vds]->partitionStartingLocation;
    dbg("Partition Length: %u\n", media->disc.udf_pd[vds]->partitionLength);

    dbg("LBN 0: LSN %u\n", lbnlsn);

    vds = -1;
    if((vds=get_correct(seq, TAG_IDENT_LVD)) < 0) {
        err("No correct LVD found. Aborting.\n");
        return ESTATUS_UNCORRECTED_ERRORS;
    }

    // Catch up on tracking minimum UDF revision required to read/write this media
    uint16_t leRecordedUDFRevision = *(const uint16_t*) media->disc.udf_lvd[vds]->domainIdent.identSuffix;
    update_min_udf_revision(stats, le16_to_cpu(leRecordedUDFRevision));
    // @todo if LVD has a Type 2 partition map, use its UDF revision to update, also

    leRecordedUDFRevision = *(const uint16_t*) media->disc.udf_iuvd[vds]->impIdent.identSuffix;
    update_min_udf_revision(stats, le16_to_cpu(leRecordedUDFRevision));

    lap = (long_ad *)media->disc.udf_lvd[vds]->logicalVolContentsUse; //FIXME BIG_ENDIAN use lela_to_cpu, but not on ptr to disc. Must store it on different place.
    lb_addr filesetblock = lap->extLocation;

    uint32_t filesetlen = lap->extLength & 0x3FFFFFFF;

    dbg("FSD at (%u, p%u)\n",
            lap->extLocation.logicalBlockNum,
            lap->extLocation.partitionReferenceNum);

    dbg("LAP: length: %x, LBN: %x, PRN: %x\n", filesetlen, filesetblock.logicalBlockNum, filesetblock.partitionReferenceNum);
    dbg("LAP: LSN: %u\n", lbnlsn/*+filesetblock.logicalBlockNum*/);

    position = (lbnlsn + filesetblock.logicalBlockNum) * stats->blocksize;
    chunk  = (uint32_t)(position / chunksize);
    offset = (uint32_t)(position % chunksize);
    map_chunk(media, chunk, __FILE__, __LINE__);

    media->disc.udf_fsd = malloc(sizeof(struct fileSetDesc));
    memcpy(media->disc.udf_fsd, media->mapping[chunk]+offset, sizeof(struct fileSetDesc));

    if (le16_to_cpu(media->disc.udf_fsd->descTag.tagIdent) != TAG_IDENT_FSD) {
        err("Error identifying FSD. Tag ID: 0x%x\n", media->disc.udf_fsd->descTag.tagIdent);
        free(media->disc.udf_fsd);
        unmap_chunk(media, chunk);
        return ESTATUS_OPERATIONAL_ERROR;
    }

    leRecordedUDFRevision = *(const uint16_t*) media->disc.udf_fsd->domainIdent.identSuffix;
    update_min_udf_revision(stats, le16_to_cpu(leRecordedUDFRevision));

    size_t ident_max_size = sizeof(media->disc.udf_fsd->logicalVolIdent);
    stats->partitionIdent = calloc(2, ident_max_size);
    if (stats->partitionIdent) {
        decode_string(NULL, media->disc.udf_fsd->logicalVolIdent, stats->partitionIdent,
                      ident_max_size, 2*ident_max_size);
        dbg("LogicVolIdent: %s\n", stats->partitionIdent);
    }

    if (verbosity >= DBG) {
        ident_max_size = sizeof(media->disc.udf_fsd->fileSetIdent);
        char *identbuf = calloc(2, ident_max_size);
        if (identbuf) {
            memset(identbuf, 0, 2*ident_max_size);
            decode_string(NULL, media->disc.udf_fsd->fileSetIdent, identbuf,
                          ident_max_size, 2*ident_max_size);
            dbg("FileSetIdent:  %s\n", identbuf);
            free(identbuf);
        }
    }

    increment_used_space(stats, filesetlen, lap->extLocation.logicalBlockNum);

    stats->lbnlsn = lbnlsn;

    unmap_chunk(media, chunk);

    stats->dstringFSDLogVolIdentErr        = check_dstring(media->disc.udf_fsd->logicalVolIdent,   128);
    stats->dstringFSDFileSetIdentErr       = check_dstring(media->disc.udf_fsd->fileSetIdent,       32);
    stats->dstringFSDCopyrightFileIdentErr = check_dstring(media->disc.udf_fsd->copyrightFileIdent, 32);
    stats->dstringFSDAbstractFileIdentErr  = check_dstring(media->disc.udf_fsd->abstractFileIdent,  32);

    dbg("Stream Length: %u\n", media->disc.udf_fsd->streamDirectoryICB.extLength & 0x3FFFFFFF);

#if HEXPRINT
    print_hex_array(media->disc.udf_fsd, sizeof(struct fileSetDesc));
#endif

    return ESTATUS_OK;
}

/**
 * \brief Inspect AED and append its allocation descriptors to an expanded version
 * of the specified array.
 *
 * \param[in]      media            Information regarding medium & access to it
 * \param[in]      aedlbn           LBN of AED
 * \param[in,out]  *lengthADArray   size of allocation descriptor array ADArray
 * \param[in,out]  **ADAarray       allocation descriptors array itself (heap memory)
 * \param[in]      *stats           file system status
 * \param[out]     status           error status
 *
 * \return 0 -- AED found and ADArray is set
 * \return 2 -- Heap allocation failed
 * \return 4 -- AED not found
 * \return 4 -- checksum failed
 * \return 4 -- CRC failed
 */
static uint8_t inspect_aed(udf_media_t *media,
                           uint32_t aedlbn, uint32_t *lengthADArray, uint8_t **ADArray,
                           struct filesystemStats *stats, uint8_t *status) {
    uint32_t lad = 0;
    uint32_t offset = 0, chunk = 0, chunksize = CHUNK_SIZE;
    uint64_t position;

    position = (stats->lbnlsn + aedlbn) * stats->blocksize;
    chunk  = (uint32_t) (position / chunksize);
    offset = (uint32_t) (position % chunksize);
    dbg("Chunk: %u, offset: 0x%x\n", chunk, offset);
    map_chunk(media, chunk, __FILE__, __LINE__);

    struct allocExtDesc *aed = (struct allocExtDesc *)(media->mapping[chunk]+offset);
    if(aed->descTag.tagIdent == TAG_IDENT_AED) {
        //checksum
        if(!checksum(aed->descTag)) {
            err("AED checksum failed\n");
            *status |= ESTATUS_UNCORRECTED_ERRORS;
            return 4;
        }

        //CRC
        if(crc(aed, aed->descTag.descCRCLength + sizeof(tag))) {
            err("AED CRC failed\n");
            *status |= ESTATUS_UNCORRECTED_ERRORS;
            return 4;
        }

        // position
        if(check_position(aed->descTag, aedlbn)) {
            err("AED position differs\n");
            *status |= ESTATUS_UNCORRECTED_ERRORS;
        }

        uint32_t L_AD = aed->lengthAllocDescs;
        uint8_t *newADArray = realloc(*ADArray, *lengthADArray + L_AD);
        if (!newADArray) {
            err("AED realloc failed\n");
            return 2;
        }
        memcpy(newADArray + *lengthADArray, (uint8_t *)(aed)+sizeof(struct allocExtDesc), L_AD);
        *ADArray = newADArray;
        *lengthADArray += L_AD;
#if 0  //For debug purposes only
        uint32_t line = 0;
        dbg("AED Array\n");
        for(int i=0; i<*lengthADArray; ) {
            note("[%04u] ",line++);
            for(int j=0; j<8; j++, i++) {
                note("%02x ", (*ADArray)[i]);
            }
            note("\n");
        }
#endif
#ifdef MEMTRACE
        dbg("ADArray ptr: %p\n", *ADArray);
#endif
        dbg("lengthADArray: %u\n", *lengthADArray);
        increment_used_space(stats, stats->blocksize, aedlbn);
        return 0;
    } else {
        err("Expected AED in LSN %u, but did not find one.\n", stats->lbnlsn + aedlbn);
    }
    return 4;
}

/**
 * \brief Collect all extents for an ICB into a contiguous heap-allocated array.
 *
 * Note, as part of the collection process, any "chain" extents marked
 * EXT_NEXT_EXTENT_ALLOCDECS are followed but are collapsed out of the array
 * returned to the caller.
 *
 * \param[in]   media              Information regarding medium & access to it
 * \param[in]   *feAllocDescs      allocation descriptors for the directory contents, in FE/EFE
 *                                 This is a pointer to memory mapped directly from the device
 * \param[in]   lengthAllocDescs   length of feAllocDescs area in bytes
 * \param[in]   icb_ad             type of AD
 * \param[out]  *ADArray           heap-allocated memory containing all ADs for the ICB (including those in chained AEDs)
 * \param[out]  *nAD               number of ADs stored at *ADArray
 * \param[in]   *stats             file system status
 * \param[out]   *status           run status
 *
 * \return 0 -- everything OK
 * \return 1 -- Unsupported AD
 * \return 2 -- Heap allocation failed
 * \return 255 -- inspect_aed() failed
 */
static uint8_t collect_extents(udf_media_t *media,
                               const uint8_t *feAllocDescs, uint32_t lengthAllocDescs,
                               uint16_t icb_ad, uint8_t **ADArray, int *nAD,
                               struct filesystemStats *stats, uint8_t *status)
{
    uint32_t descSize = 0;

    switch(icb_ad) {
        case ICBTAG_FLAG_AD_SHORT:
            dbg("Short AD\n");
            descSize = sizeof(short_ad);
            break;
        case ICBTAG_FLAG_AD_LONG:
            dbg("Long AD\n");
            descSize = sizeof(long_ad);
            break;
        case ICBTAG_FLAG_AD_EXTENDED:
            dbg("Extended AD\n");
            descSize = sizeof(ext_ad);
            break;
        default:
            err("[collect_extents] Unsupported icb_ad: 0x%04x\n", icb_ad);
            return 1;
    }
    dbg("LengthOfAllocDescs: %u\n", lengthAllocDescs);


    // Collect all of the ICB's allocation descriptors into a single array
    uint32_t lengthADArray = lengthAllocDescs;
    *ADArray = malloc(lengthAllocDescs);

    if (!*ADArray) {
        err("AD allocation failed.\n");
        *nAD = 0;
        return 2;
    }

    memcpy(*ADArray, feAllocDescs, lengthAllocDescs);
    *nAD = lengthAllocDescs / descSize;

    for(int i = 0; i < *nAD; i++) {
        uint32_t aedlbn = 0;
        short_ad *sad = (short_ad *)(*ADArray + i*descSize); //we can do that, because all ADs have size as first.

        // ECMA 167r3 sec. 12: AD with zero extent length terminates the sequence
        if (!(sad->extLength & 0x3FFFFFFF)) {
            // @todo Something if i != (nAD - 1).
            // Not so easy with current implementation because we've tossed the lbn (AED or FE or EFE)
            // (easy enough to fix) and don't have the nAD from that block
            break;
        }
        uint32_t extType = sad->extLength >> 30;
        dbg("ExtLength: %u, type: %u\n", sad->extLength & 0x3FFFFFFF, extType);
        if(extType == 3) { //Extent is AED
            long_ad *lad;
            ext_ad *ead;
            switch(icb_ad) {
                case ICBTAG_FLAG_AD_SHORT:
                    //we already have sad
                    aedlbn = sad->extPosition;
                    break;
                case ICBTAG_FLAG_AD_LONG:
                    lad = (long_ad *)(*ADArray + i*descSize);
                    aedlbn = lad->extLocation.logicalBlockNum;
                    break;
                case ICBTAG_FLAG_AD_EXTENDED:
                    ead = (ext_ad *)(*ADArray + i*descSize);
                    aedlbn = ead->extLocation.logicalBlockNum;
                    break;
            }
            // Erase the chain entry just in case the chained AED has zero entries
            memset(*ADArray + i*descSize, 0, descSize);

            lengthADArray -= descSize;  // Force this (chain) entry to be overwritten
            if (inspect_aed(media, aedlbn, &lengthADArray, ADArray, stats, status)) {
                err("AED inspection failed.\n");
                return 255;
            }

            *nAD = (lengthADArray / descSize);

            // Force rescan of the current ADArray entry.
            // It has changed from a chain to the next AED
            // to the first AD in that next AED.
            --i;
        }  // AED extent
    }

    return 0;
}

/**
 * \brief Parse the contents of a directory given the allocation descriptors within its FE/EFE.
 *
 * Note, the contents can be split across extents, even in the middle of a file information descriptor.
 * This function creates a virtual linear area for further processing.
 *
 * This function internally calls inspect_fid().
 *
 * \param[in]   media              Information regarding medium & access to it
 * \param[in]   lsn                actual LSN
 * \param[in]   *allocDescs        allocation descriptors for the directory contents, in FE/EFE
 * \param[in]   lengthAllocDescs   length of allocation descriptors area in bytes
 * \param[in]   icb_ad             type of AD
 * \param[in]   *stats             file system status
 * \param[in]   depth              depth of FE for printing
 * \param[in]   *seq               VDS sequence
 * \param[out]  *status            run status
 *
 * \return 0 -- everything OK
 * \return 1 -- Unsupported AD
 * \return 2 -- Heap allocation failed
 * \return 255 -- inspect_aed() failed
 */
static uint8_t walk_directory(udf_media_t *media,
                              uint32_t lsn, uint8_t *allocDescs,
                              uint32_t lengthAllocDescs, uint16_t icb_ad,
                              struct filesystemStats *stats, uint32_t depth, vds_sequence_t *seq,
                              uint8_t *status) {

    uint32_t descSize = 0;
    uint8_t *dirContent = NULL;
    int nAD = 0;
    uint64_t dirContentLen = 0;
    short_ad *sad = NULL;
    long_ad *lad = NULL;
    ext_ad *ead = NULL;
    uint32_t offset = 0, chunk = 0, chunksize = CHUNK_SIZE;
    uint64_t position = 0;

    uint32_t lengthADArray = 0;
    uint8_t *ADArray = NULL;

    // Collect all of the ICB's allocation descriptors into a single array
    int extentErr = collect_extents(media, allocDescs, lengthAllocDescs, icb_ad,
                                    &ADArray, &nAD, stats, status);
    if (extentErr) {
        free(ADArray);
        return extentErr;
    }

    switch(icb_ad) {
        case ICBTAG_FLAG_AD_SHORT:
            dbg("Short AD\n");
            descSize = sizeof(short_ad);
            break;
        case ICBTAG_FLAG_AD_LONG:
            dbg("Long AD\n");
            descSize = sizeof(long_ad);
            break;
        case ICBTAG_FLAG_AD_EXTENDED:
            dbg("Extended AD\n");
            descSize = sizeof(ext_ad);
            break;
        default:
            err("[walk_directory] Unsupported icb_ad: 0x%04x\n", icb_ad);
            free(ADArray);
            return 1;
    }

    for(int i = 0; i < nAD; i++) {
        sad = (short_ad *)(ADArray + i*descSize); //we can do that, because all ADs have size as first.

        dirContentLen += sad->extLength & 0x3FFFFFFF;
    }

    dbg("Dir content length: %u\n", dirContentLen);
    dbg("nAD: %u\n", nAD);

    // Now read the entire directory contents into a contiguous array

    dirContent = calloc(1, dirContentLen);
    if(dirContent == NULL) {
        err("Dir content allocation failed.\n");
        free(ADArray);
        return 2;
    }

    uint32_t prevExtLength = 0;
    for(int i = 0; i < nAD; i++) {
        uint32_t extStartLBN;
        uint32_t extType;
        uint32_t extLength;
        switch(icb_ad) {
            case ICBTAG_FLAG_AD_SHORT:
                sad = (short_ad *)(ADArray + i*descSize);
                extType     = sad->extLength >> 30;
                extLength   = sad->extLength & 0x3FFFFFFF;
                extStartLBN = sad->extPosition;
                break;

            case ICBTAG_FLAG_AD_LONG:
                lad = (long_ad *)(ADArray + i*descSize);
                extType     = lad->extLength >> 30;
                extLength   = lad->extLength & 0x3FFFFFFF;
                extStartLBN = lad->extLocation.logicalBlockNum;
                break;

            case ICBTAG_FLAG_AD_EXTENDED:
                ead = (ext_ad *)(ADArray + i*descSize);
                extType     = ead->extLength >> 30;
                extLength   = ead->extLength & 0x3FFFFFFF;
                extStartLBN = ead->extLocation.logicalBlockNum;
                break;

            default:
                // @todo something
                continue;
                break;
        }

        if (extType == 0) {
            // Allocated and Recorded
            position = (stats->lbnlsn + extStartLBN) * stats->blocksize;
            chunk  = (uint32_t)(position / chunksize);
            offset = (uint32_t)(position % chunksize);
            dbg("Chunk: %u, offset: 0x%x\n", chunk, offset);
            map_chunk(media, chunk, __FILE__, __LINE__);

            memcpy(dirContent+prevExtLength, (uint8_t *)(media->mapping[chunk] + offset), extLength);
        } else {
            // Not recorded
            memset(dirContent+prevExtLength, 0, extLength);
        }
        if (extType != 2) {
            // Allocated
            increment_used_space(stats, 1, extStartLBN);
        }
        prevExtLength += extLength;
    }

    uint8_t tempStatus = 0;
    int counter = 0;
    for(uint32_t pos=0; pos < dirContentLen; ) {
        dbg("FID #%d\n", counter++);
        if (inspect_fid(media, lsn, dirContent, &pos, stats, depth+1, seq, &tempStatus) != 0) {
            dbg("1 FID inspection over.\n");
            break;
        }
    } 
    dbg("2 FID inspection over.\n");

    if(tempStatus & ESTATUS_CORRECTED_ERRORS) { // FID(s) were fixed - write dirContent back out
        prevExtLength = 0;
        for(int i = 0; i < nAD; i++) {
            uint32_t extStartLBN;
            uint32_t extType;
            uint32_t extLength;
            switch(icb_ad) {
                case ICBTAG_FLAG_AD_SHORT:
                    sad = (short_ad *)(ADArray + i*descSize);
                    extType     = sad->extLength >> 30;
                    extLength   = sad->extLength & 0x3FFFFFFF;
                    extStartLBN = sad->extPosition;
                    break;

                case ICBTAG_FLAG_AD_LONG:
                    lad = (long_ad *)(ADArray + i*descSize);
                    extType     = lad->extLength >> 30;
                    extLength   = lad->extLength & 0x3FFFFFFF;
                    extStartLBN = lad->extLocation.logicalBlockNum;
                    break;

                case ICBTAG_FLAG_AD_EXTENDED:
                    ead = (ext_ad *)(ADArray + i*descSize);
                    extType     = ead->extLength >> 30;
                    extLength   = ead->extLength & 0x3FFFFFFF;
                    extStartLBN = ead->extLocation.logicalBlockNum;
                    break;
            }

            if (extType == 0) {
                // Allocated and Recorded
                position = (stats->lbnlsn + extStartLBN) * stats->blocksize;
                chunk  = (uint32_t)(position / chunksize);
                offset = (uint32_t)(position % chunksize);
                dbg("Chunk: %u, offset: 0x%x\n", chunk, offset);
                map_chunk(media, chunk, __FILE__, __LINE__);

                memcpy(media->mapping[chunk] + offset, dirContent + prevExtLength, extLength);
            }
            // else @todo, depends how unrecorded extents were handled earlier

            prevExtLength += extLength;
        }   // for each extent of directory contents

        dbg("3 directory copyback done.\n");
    }  // if directory contents need writeback


    //free arrays
    free(dirContent);
    free(ADArray);
    (*status) |= tempStatus;
    return 0;
}

/**
 * \brief FID parsing function
 *
 * This function parses a FID. It continues to its FE using get_file() function.
 * Checks and fixes *Unique ID*, *Serial Numbers* or unfinished writing.
 *
 * This function is a complement to get_file() and translate_fid().
 *
 * \param[in]     media     Information regarding medium & access to it
 * \param[in]     lsn       actual LSN
 * \param[in]     *base     base pointer for for FID area
 * \param[in,out] *pos      actual position in FID area
 * \param[in]     *stats    file system status
 * \param[in]     depth     depth of FE for printing
 * \param[in]     *seq      VDS sequence
 * \param[out]     *status  run status
 *
 * \return 0 -- everything OK
 * \return 1 -- Unknown descriptor found
 * \return 252 -- FID checksum failed
 * \return 251 -- FID CRC failed
 */
uint8_t inspect_fid(udf_media_t *media,
                    uint32_t lsn, uint8_t *base, uint32_t *pos,
                    struct filesystemStats *stats, uint32_t depth, vds_sequence_t *seq,
                    uint8_t *status) {
    uint32_t flen, padding;
    struct fileIdentDesc *fid = (struct fileIdentDesc *)(base + *pos);
    struct fileInfo info;
    memset(&info, 0, sizeof(struct fileInfo));
    uint32_t offset = 0, chunk = 0;
    uint64_t position = 0;
    uint32_t chunksize = CHUNK_SIZE;

    dbg("FID pos: 0x%x\n", *pos);
    if (!checksum(fid->descTag)) {
        err("[inspect fid] FID checksum failed.\n");
        return -4;
        warn("DISABLED ERROR RETURN\n");
    }
    if (le16_to_cpu(fid->descTag.tagIdent) == TAG_IDENT_FID) {
        dwarn("FID found (%u)\n",*pos);
        flen = 38 + le16_to_cpu(fid->lengthOfImpUse) + fid->lengthFileIdent;
        padding = 4 * ((le16_to_cpu(fid->lengthOfImpUse) + fid->lengthFileIdent + 38 + 3)/4) - (le16_to_cpu(fid->lengthOfImpUse) + fid->lengthFileIdent + 38);

        dbg("lengthOfImpUse: %u\n", fid->lengthOfImpUse);
        dbg("flen+padding: %u\n", flen+padding);
        if(crc(fid, flen + padding)) {
            err("FID CRC failed.\n");
            return -5;
            warn("DISABLED ERROR RETURN\n");
        }
        dbg("FID: ImpUseLen: %u\n", fid->lengthOfImpUse);
        dbg("FID: FilenameLen: %u\n", fid->lengthFileIdent);
        if(fid->lengthFileIdent == 0) {
            dbg("ROOT directory\n");
        } else {
            char *namebuf = calloc(1,256*2);
            memset(namebuf, 0, 256*2);
            uint8_t *fileIdent = fid->impUseAndFileIdent + fid->lengthOfImpUse;
            size_t size = decode_utf8(fileIdent, namebuf, fid->lengthFileIdent, 256*2);
            if(size == (size_t) - 1) { //Decoding failed
                warn("Filename decoding failed."); //TODO add tests
            } else {
                dbg("Size: %zu\n", size);
                dbg("%sFilename: %s\n", depth2str(depth), namebuf/*fid->fileIdent*/);
                info.filename = namebuf/*(char *)fid->fileIdent+1*/;
            }
        }

        dbg("Tag Serial Num: %u\n", fid->descTag.tagSerialNum);
        if(stats->AVDPSerialNum != fid->descTag.tagSerialNum) {
            err("(%s) Tag Serial Number differs.\n", info.filename);
            uint8_t fixsernum = autofix;
            if(interactive) {
                if(prompt("Fix it? [Y/n] ")) {
                    fixsernum = 1;
                }
            }
            if(fixsernum) {
                fid->descTag.tagSerialNum = stats->AVDPSerialNum;
                fid->descTag.descCRC = calculate_crc(fid, flen+padding);             
                fid->descTag.tagChecksum = calculate_checksum(fid->descTag);

                position = lsn * stats->blocksize;
                chunk  = (uint32_t)(position / chunksize);
                offset = (uint32_t)(position % chunksize);
                dbg("Chunk: %u, offset: 0x%x\n", chunk, offset);
                map_chunk(media, chunk, __FILE__, __LINE__);

                struct fileEntry *fe = (struct fileEntry *)(media->mapping[chunk] + offset);
                struct extendedFileEntry *efe = (struct extendedFileEntry *)fe;
                if(efe->descTag.tagIdent == TAG_IDENT_EFE) {
                    efe->descTag.descCRC = calculate_crc(efe, sizeof(struct extendedFileEntry) + le32_to_cpu(efe->lengthExtendedAttr) + le32_to_cpu(efe->lengthAllocDescs));
                    efe->descTag.tagChecksum = calculate_checksum(efe->descTag);
                    dbg("[CHECKSUM] %"PRIx16"\n", efe->descTag.tagChecksum);
                } else if(efe->descTag.tagIdent == TAG_IDENT_FE) {
                    fe->descTag.descCRC = calculate_crc(fe, sizeof(struct fileEntry) + le32_to_cpu(fe->lengthExtendedAttr) + le32_to_cpu(fe->lengthAllocDescs));
                    fe->descTag.tagChecksum = calculate_checksum(fe->descTag);
                    dbg("[CHECKSUM] %"PRIx16"\n", fe->descTag.tagChecksum);
                } else {
                    err("(%s) FID parent FE not found.\n", info.filename);
                }
                imp("(%s) Tag Serial Number was fixed.\n", info.filename);
                sync_chunk(media->mapping, chunk, media->devsize);
                *status |= ESTATUS_CORRECTED_ERRORS;
            } else {
                *status |= ESTATUS_UNCORRECTED_ERRORS;
            }
        }

        dbg("FileVersionNum: %u\n", fid->fileVersionNum);

        info.fileCharacteristics = fid->fileCharacteristics;
        if((fid->fileCharacteristics & FID_FILE_CHAR_DELETED) == 0) { //NOT deleted, continue
            dbg("ICB: LSN: %u, length: %u\n", fid->icb.extLocation.logicalBlockNum + stats->lbnlsn,
                fid->icb.extLength);
            dbg("ROOT ICB: LSN: %u\n",
                media->disc.udf_fsd->rootDirectoryICB.extLocation.logicalBlockNum + stats->lbnlsn);

            if(*pos == 0) {
                dbg("Parent. Not Following this one\n");
            } else if ((fid->icb.extLocation.logicalBlockNum + stats->lbnlsn) == lsn) {
                dbg("Self. Not following this one\n");
            } else if (   (fid->icb.extLocation.logicalBlockNum + stats->lbnlsn)
                       == (media->disc.udf_fsd->rootDirectoryICB.extLocation.logicalBlockNum + stats->lbnlsn)) {
                dbg("ROOT. Not following this one.\n");
            } else {
                uint32_t uuid = 0;
                memcpy(&uuid, (fid->icb).impUse+2, sizeof(uint32_t));
                dbg("UUID: %u\n", uuid);
                if(stats->found.nextUID <= uuid) {
                    stats->found.nextUID = uuid + 1;
                    dwarn("New MAX UUID\n");
                }
                int fixuuid = 0;
                if ((uuid == 0) && (stats->found.minUDFReadRev > 0x0200)) {
                    err("(%s) FID Unique ID is 0. Next available is %" PRIu64 ".\n", info.filename,
                        stats->lvid.nextUID);
                    if(interactive) {
                        if(prompt("Fix it? [Y/n] ")) {
                            fixuuid = 1;
                        } else {
                            *status |= ESTATUS_UNCORRECTED_ERRORS;
                        }
                    }       
                    if(autofix) {
                        fixuuid = 1;
                    } else {
                        *status |= ESTATUS_UNCORRECTED_ERRORS;
                    }
                    if(fixuuid) {
// @todo This is a problem. It assumes LVID nextUID is accurate, which may not be so even if LVID is present.
// Probably need to make two passes, one to map out the highest known UID, the other to assign new ones.
// Another option would be to assign way-huge UIDs in the first pass and clean them up in the second.
                        uuid = stats->lvid.nextUID;
                        stats->found.nextUID = uuid;
                        stats->lvid.nextUID++;
                        seq->lvid.error |= E_UUID;
                        fid->icb.impUse[2] = uuid;
                        fid->descTag.descCRC = calculate_crc(fid, flen+padding);             
                        fid->descTag.tagChecksum = calculate_checksum(fid->descTag);
                        dbg("Location: %u\n", fid->descTag.tagLocation);

                        position = lsn * stats->blocksize;
                        chunk  = (uint32_t)(position / chunksize);
                        offset = (uint32_t)(position % chunksize);
                        dbg("Chunk: %u, offset: 0x%x\n", chunk, offset);
                        map_chunk(media, chunk, __FILE__, __LINE__);

                        struct fileEntry *fe = (struct fileEntry *)(media->mapping[chunk] + offset);
                        struct extendedFileEntry *efe = (struct extendedFileEntry *)fe;
                        if(efe->descTag.tagIdent == TAG_IDENT_EFE) {
                            efe->descTag.descCRC = calculate_crc(efe, sizeof(struct extendedFileEntry) + le32_to_cpu(efe->lengthExtendedAttr) + le32_to_cpu(efe->lengthAllocDescs));
                            efe->descTag.tagChecksum = calculate_checksum(efe->descTag);
                        } else if(efe->descTag.tagIdent == TAG_IDENT_FE) {
                            fe->descTag.descCRC = calculate_crc(fe, sizeof(struct fileEntry) + le32_to_cpu(fe->lengthExtendedAttr) + le32_to_cpu(fe->lengthAllocDescs));
                            fe->descTag.tagChecksum = calculate_checksum(fe->descTag);
                        } else {

                        }
                        imp("(%s) UUID was fixed.\n", info.filename);
                        *status |= ESTATUS_CORRECTED_ERRORS;
                    }
                }
                dbg("ICB to follow.\n");
                int tmp_status = get_file(media,
                                          fid->icb.extLocation.logicalBlockNum + stats->lbnlsn, stats,
                                          depth, uuid, info, seq);
                if(tmp_status == 32) { //32 means delete this FID
                    fid->fileCharacteristics |= FID_FILE_CHAR_DELETED; //Set deleted flag
                    memset(&(fid->icb), 0, sizeof(long_ad)); //clear ICB according to ECMA-167r3, 4/14.4.5
                    fid->descTag.descCRC = calculate_crc(fid, flen+padding);             
                    fid->descTag.tagChecksum = calculate_checksum(fid->descTag);
                    dbg("Location: %u\n", fid->descTag.tagLocation);

                    position = (fid->descTag.tagLocation + stats->lbnlsn) * stats->blocksize;
                    chunk  = (uint32_t)(position / chunksize);
                    offset = (uint32_t)(position % chunksize);
                    dbg("Chunk: %u, offset: 0x%x\n", chunk, offset);
                    map_chunk(media, chunk, __FILE__, __LINE__);

                    struct fileEntry *fe = (struct fileEntry *)(media->mapping[chunk] + offset);
                    struct extendedFileEntry *efe = (struct extendedFileEntry *)fe;
                    if(efe->descTag.tagIdent == TAG_IDENT_EFE) {
                        efe->descTag.descCRC = calculate_crc(efe, sizeof(struct extendedFileEntry) + le32_to_cpu(efe->lengthExtendedAttr) + le32_to_cpu(efe->lengthAllocDescs));
                        efe->descTag.tagChecksum = calculate_checksum(efe->descTag);
                    } else if(efe->descTag.tagIdent == TAG_IDENT_EFE) {
                        fe->descTag.descCRC = calculate_crc(fe, sizeof(struct fileEntry) + le32_to_cpu(fe->lengthExtendedAttr) + le32_to_cpu(fe->lengthAllocDescs));
                        fe->descTag.tagChecksum = calculate_checksum(fe->descTag);
                    } else {
                        err("(%s) FID parent FE not found.\n", info.filename);
                    }
                    imp("(%s) Unfinished file was removed.\n", info.filename);

                    tmp_status = ESTATUS_CORRECTED_ERRORS;
                }
                *status |= tmp_status;
                dbg("Return from ICB\n"); 
            }
        } else {
            dbg("DELETED FID\n");
            uint8_t *fileIdent = fid->impUseAndFileIdent + fid->lengthOfImpUse;
            *status |= check_dstring(fileIdent, fid->lengthFileIdent) ? ESTATUS_UNCORRECTED_ERRORS
                                                                      : ESTATUS_OK; //FIXME expand for fixing later.
            print_file_info(info, depth);
        }
        dbg("Len: %u, padding: %u\n", flen, padding);
        *pos = *pos + flen + padding;
        note("\n");
    } else {
        msg("Ident: %x\n", le16_to_cpu(fid->descTag.tagIdent));
        uint8_t *fidarray = (uint8_t *)fid;
        for(int i=0; i<80;) {
            for(int j=0; j<8; j++, i++) {
                note("%02x ", fidarray[i]);
            }
            note("\n");
        }
        return 1;
    }

    free(info.filename);
    return 0;
}

uint32_t get_used_blocks(const integrity_info_t *info)
{
    return (info->partitionNumBlocks - info->freeSpaceBlocks);
}

static void update_min_udf_revision(struct filesystemStats *stats, uint16_t new_revision)
{
    if (new_revision > stats->found.minUDFReadRev)
        stats->found.minUDFReadRev = new_revision;

    if (new_revision > stats->found.minUDFWriteRev)
        stats->found.minUDFWriteRev = new_revision;
}

/**
 * \brief Pair function capturing used space and its position
 *
 * This function is a dual of decrement_used_space()
 * 
 * It only stores information about used:free space ration and positions
 *
 * \param[in,out] *stats file system status contains fields used for free space counting and bitmaps for position marking
 * \param[in] increment  number of bytes of space to mark
 * \param[in] its position
 */
void increment_used_space(struct filesystemStats *stats, uint64_t increment, uint32_t position) {
    uint32_t increment_blocks = (increment + stats->blocksize - 1) / stats->blocksize;
    stats->found.freeSpaceBlocks -= increment_blocks;
    markUsedBlock(stats, position, increment_blocks, MARK_BLOCK);
#if DEBUG
    dwarn("INCREMENT to %u\n", get_used_blocks(&stats->found));
#endif
}

/**
 * \brief Pair function capturing used space and its position
 *
 * This function is a dual of increment_used_space()
 * 
 * It only stores information about used:free space ration and positions
 *
 * \param[in,out] *stats file system status contains fields used for free space counting and bitmaps for position marking
 * \param[in] decrement number of bytes of space to mark
 * \param[in] its position
 */
void decrement_used_space(struct filesystemStats *stats, uint64_t decrement, uint32_t position) {
    uint32_t decrement_blocks = (decrement + stats->blocksize - 1) / stats->blocksize;
    stats->found.freeSpaceBlocks += decrement_blocks;
    markUsedBlock(stats, position, decrement_blocks, UNMARK_BLOCK);
#if DEBUG
    dwarn("DECREMENT to %u\n", get_used_blocks(&stats->found));
#endif
}

/**
 * \brief (E)FE parsing function
 *
 * This function parses thru file tree, made of FE. It complements inspect_fid(), which parses FIDs.
 * 
 * It fixes *Unfinished writes*, *File modification timestamps* (or records them for LVID fix, depending on error) and *Unique ID*.
 *
 * When it finds a directory, it calls walk_directory() to process its contents.
 *
 * \param[in]      media     Information regarding medium & access to it
 * \param[in]      lsn       actual LSN
 * \param[in,out]  *stats    file system status
 * \param[in]      depth     depth of FE for printing
 * \param[in]      uuid      Unique ID from parent FID
 * \param[in]      info      file information structure for easier handling for print
 * \param[in]      *seq      VDS sequence
 *
 * \return 4 -- No correct LVD found
 * \return 4 -- Checksum failed
 * \return 4 -- CRC failed 
 * \return 32 -- removed unfinished file
 * \return sum of status returned from inspect_fid(), translate_fid() or own actions (4 for unfixed error, 1 for fixed error, 0 for no error)
 */
uint8_t get_file(udf_media_t *media,
                 uint32_t lsn, struct filesystemStats *stats, uint32_t depth,
                 uint32_t uuid, struct fileInfo info, vds_sequence_t *seq ) {
    tag *descTag;
    struct fileEntry *fe;
    struct extendedFileEntry *efe;
    int vds = -1;

    if((vds=get_correct(seq, TAG_IDENT_LVD)) < 0) {
        err("No correct LVD found. Aborting.\n");
        return ESTATUS_UNCORRECTED_ERRORS;
    }

    uint8_t dir = 0;
    uint8_t status = 0;
    uint32_t chunksize = CHUNK_SIZE;
    uint32_t chunk = 0;
    uint32_t offset = 0;
    uint64_t position = 0;

    dwarn("\n(%u) ---------------------------------------------------\n", lsn);
    position = stats->blocksize * lsn;
    chunk  = (uint32_t)(position / chunksize);
    offset = (uint32_t)(position % chunksize);
    dbg("Chunk: %u, offset: 0x%x\n", chunk, offset);
    map_chunk(media, chunk, __FILE__, __LINE__);

    descTag = (tag *)(media->mapping[chunk] + offset);
    if(!checksum(*descTag)) {
        err("Tag checksum failed. Unable to continue.\n");
        return ESTATUS_UNCORRECTED_ERRORS;
    }

    dbg("global FE increment.\n");
    dbg("usedSpace: %u\n", get_used_blocks(&stats->found));
    increment_used_space(stats, stats->blocksize, lsn - stats->lbnlsn);
    dbg("usedSpace: %u\n", get_used_blocks(&stats->found));
    switch(le16_to_cpu(descTag->tagIdent)) {
        case TAG_IDENT_FE:
        case TAG_IDENT_EFE:
            dir = 0;
            fe = (struct fileEntry *)(media->mapping[chunk] + offset);
            efe = (struct extendedFileEntry *)fe;
            uint8_t ext = 0;

            if(le16_to_cpu(descTag->tagIdent) == TAG_IDENT_EFE) {
                dwarn("[EFE]\n");
                if(crc(efe, sizeof(struct extendedFileEntry) + le32_to_cpu(efe->lengthExtendedAttr) + le32_to_cpu(efe->lengthAllocDescs))) {
                    err("EFE CRC failed.\n");
                    int cont = 0;
                    if(interactive) {
                        if(prompt("Continue with caution, yes? [Y/n] ")) {
                            cont = 1;
                        }
                    }
                    if(cont == 0) {
                        unmap_chunk(media, chunk);
                        return ESTATUS_UNCORRECTED_ERRORS;
                    }
                }
                update_min_udf_revision(stats, 0x0200);
                ext = 1;
            } else {
                if(crc(fe, sizeof(struct fileEntry) + le32_to_cpu(fe->lengthExtendedAttr) + le32_to_cpu(fe->lengthAllocDescs))) {
                    err("FE CRC failed.\n");
                    int cont = 0;
                    if(interactive) {
                        if(prompt("Continue with caution, yes? [Y/n] ")) {
                            cont = 1;
                        }
                    }
                    if(cont == 0) {
                        unmap_chunk(media, chunk);
                        return ESTATUS_UNCORRECTED_ERRORS;
                    }
                }
            }
            dbg("Tag Serial Num: %u\n", descTag->tagSerialNum);
            if(stats->AVDPSerialNum != descTag->tagSerialNum) {
                err("(%s) Tag Serial Number differs.\n", info.filename);
                uint8_t fixsernum = autofix;
                if(interactive) {
                    if(prompt("Fix it? [Y/n] ")) {
                        fixsernum = 1;
                    }
                }
                if(fixsernum) {
                    descTag->tagSerialNum = stats->AVDPSerialNum;
                    if(ext) {
                        descTag->descCRC = calculate_crc(efe, sizeof(struct extendedFileEntry) + le32_to_cpu(efe->lengthExtendedAttr) + le32_to_cpu(efe->lengthAllocDescs));
                    } else {
                        descTag->descCRC = calculate_crc(fe, sizeof(struct fileEntry) + le32_to_cpu(fe->lengthExtendedAttr) + le32_to_cpu(fe->lengthAllocDescs));
                    }
                    descTag->tagChecksum = calculate_checksum(*descTag);
                    status |= ESTATUS_CORRECTED_ERRORS;
                } else {
                    status |= ESTATUS_UNCORRECTED_ERRORS;
                }
            }
            dbg("\nFE, LSN: %u, EntityID: %s ", lsn, fe->impIdent.ident);
            dbg("fileLinkCount: %u, LB recorded: %" PRIu64 "\n", fe->fileLinkCount,
                ext ? efe->logicalBlocksRecorded: fe->logicalBlocksRecorded);
            uint32_t L_EA = ext ? efe->lengthExtendedAttr : fe->lengthExtendedAttr;
            uint32_t L_AD = ext ? efe->lengthAllocDescs   : fe->lengthAllocDescs;
            dbg("L_EA %u, L_AD %u\n", L_EA, L_AD);
            dbg("Information Length: %" PRIu64 "\n", fe->informationLength);
            uint32_t info_len_blocks = (uint32_t) (fe->informationLength / stats->blocksize);
            if ((fe->informationLength % stats->blocksize) != 0)
                info_len_blocks++;
            dbg("InfLenBlocks: %u\n", info_len_blocks);
            dbg("BlocksRecord: %" PRIu64 "\n", ext ? efe->logicalBlocksRecorded : fe->logicalBlocksRecorded);

            info.size = fe->informationLength;
            info.fileType = fe->icbTag.fileType;
            info.permissions = fe->permissions;
            dbg("Permissions: 0x%04x : 0x%04x\n", info.permissions, fe->permissions);

            switch(fe->icbTag.fileType) {
                case ICBTAG_FILE_TYPE_UNDEF:
                    dbg("Filetype: undef\n");
                    break;  
                case ICBTAG_FILE_TYPE_USE:
                    dbg("Filetype: USE\n");
                    break;  
                case ICBTAG_FILE_TYPE_PIE:
                    dbg("Filetype: PIE\n");
                    break;  
                case ICBTAG_FILE_TYPE_IE:
                    dbg("Filetype: IE\n");
                    break;  
                case ICBTAG_FILE_TYPE_DIRECTORY:
                    dbg("Filetype: DIR\n");
                    stats->found.numDirs ++;
                    // stats->usedSpace += lbSize;
                    //increment_used_space(stats, lbSize);
                    dir = 1;
                    break;  
                case ICBTAG_FILE_TYPE_REGULAR:
                    dbg("Filetype: REGULAR\n");
                    stats->found.numFiles ++;
                    //                    stats->usedSpace += lbSize;
                    break;  
                case ICBTAG_FILE_TYPE_BLOCK:
                    dbg("Filetype: BLOCK\n");
                    stats->found.numFiles ++;
                    break;  
                case ICBTAG_FILE_TYPE_CHAR:
                    dbg("Filetype: CHAR\n");
                    stats->found.numFiles ++;
                    break;  
                case ICBTAG_FILE_TYPE_EA:
                    dbg("Filetype: EA\n");
                    break;  
                case ICBTAG_FILE_TYPE_FIFO:
                    dbg("Filetype: FIFO\n");
                    stats->found.numFiles ++;
                    break;  
                case ICBTAG_FILE_TYPE_SOCKET:
                    dbg("Filetype: SOCKET\n");
                    break;  
                case ICBTAG_FILE_TYPE_TE:
                    dbg("Filetype: TE\n");
                    break;  
                case ICBTAG_FILE_TYPE_SYMLINK:
                    dbg("Filetype: SYMLINK\n");
                    stats->found.numFiles ++;
                    break;  
                case ICBTAG_FILE_TYPE_STREAMDIR:
                    dbg("Filetype: STRAMDIR\n");
                    //stats->usedSpace += lbSize;
                    break; 
                default:
                    dbg("Unknown filetype\n");
                    break; 
            }

            dbg("numEntries: %u\n", fe->icbTag.numEntries);
            dbg("Parent ICB loc: %u\n", fe->icbTag.parentICBLocation.logicalBlockNum);

            double cts = 0;
            if((cts = compare_timestamps(stats->lvid.recordedTime, ext ? efe->modificationTime : fe->modificationTime)) < 0) {
                if (!seq->lvid.error) {
                    err("(%s) File timestamp is later than LVID timestamp. LVID needs to be fixed.\n", info.filename);
#ifdef DEBUG
                    err("CTS: %f\n", cts);
#endif
                }
                seq->lvid.error |= E_TIMESTAMP; 
            }
            info.modTime = ext ? efe->modificationTime : fe->modificationTime;


            uint64_t feUUID = (ext ? efe->uniqueID : fe->uniqueID);
            dbg("Unique ID: FE: %"PRIu64" FID: %"PRIu32"\n", (feUUID), uuid); //PRIu32 is fixing uint32_t printing
            if (uuid == 0) {
                // Account UIDs that can't be handled during FID processing
                if(stats->found.nextUID <= feUUID) {
                    stats->found.nextUID = feUUID + 1;
                    dwarn("New MAX UUID\n");
                }
            }
            int fixuuid = 0;
            if ((uuid != feUUID) && (uuid != 0)) {
                err("(%s) FE Unique ID differs from FID Unique ID.\n", info.filename);
                if(interactive) {
                    if(prompt("Fix it (set Unique ID to %u, value according to FID)? [Y/n] ", uuid) != 0) {
                        fixuuid = 1;
                    } else {
                        status |= ESTATUS_UNCORRECTED_ERRORS;
                    }
                }
                if(autofix) {
                    fixuuid = 1;
                }
            }
            if(fixuuid) {
                    if(lsn==1704005)
                        dbg("[1704005] fixuuid");
                if(ext) {
                    efe->uniqueID = uuid;
                    efe->descTag.descCRC = calculate_crc(efe, sizeof(struct extendedFileEntry) + le32_to_cpu(efe->lengthExtendedAttr) + le32_to_cpu(efe->lengthAllocDescs));
                    efe->descTag.tagChecksum = calculate_checksum(efe->descTag);
                } else {
                    fe->uniqueID = uuid;
                    fe->descTag.descCRC = calculate_crc(fe, sizeof(struct fileEntry) + le32_to_cpu(fe->lengthExtendedAttr) + le32_to_cpu(fe->lengthAllocDescs));
                    fe->descTag.tagChecksum = calculate_checksum(fe->descTag);
                }
                status |= ESTATUS_CORRECTED_ERRORS;
            }

            dbg("FC: %04u DC: %04u ", stats->found.numFiles, stats->found.numDirs);
            print_file_info(info, depth);

            uint8_t fid_inspected = 0;
            uint8_t *allocDescs = (ext ? efe->extendedAttrAndAllocDescs : fe->extendedAttrAndAllocDescs) + L_EA; 
            uint16_t icbTagADFlags = le16_to_cpu(fe->icbTag.flags) & ICBTAG_FLAG_AD_MASK;
            if (   (icbTagADFlags == ICBTAG_FLAG_AD_SHORT)
                || (icbTagADFlags == ICBTAG_FLAG_AD_LONG)) {

                if(dir) {
                    fid_inspected = 1;
                    walk_directory(media, lsn, allocDescs, L_AD,
                                   icbTagADFlags, stats, depth, seq, &status);
                } else {
                    uint32_t lengthADArray = 0;
                    uint8_t *ADArray = NULL;  // Heap-allocated memory we must free
                    int nAD = 0;

                    int extentErr = collect_extents(media, allocDescs, L_AD,
                                                    icbTagADFlags, &ADArray, &nAD, stats, &status);
                    if (extentErr) {
                        nAD = 0;
                    }

                    size_t descLen;
                    if (icbTagADFlags == ICBTAG_FLAG_AD_SHORT) {
                        descLen = sizeof(short_ad);
                        dbg("SHORT\n");
                    } else {
                        descLen = sizeof(long_ad);
                        dbg("LONG\n");
                    }
                    dbg("LAD: %u, N: %u, rest: %u\n", L_AD, L_AD / descLen, L_AD % descLen);
                    for(int si = 0; si < nAD; si++) {
                        uint32_t extLength;
                        uint32_t extType;
                        uint32_t extPosition;

                        if (icbTagADFlags == ICBTAG_FLAG_AD_SHORT) {
                            dwarn("SHORT #%d\n", si);
                            short_ad *sad = (short_ad *)(ADArray + si*descLen);
                            extLength   = sad->extLength & 0x3FFFFFFF;
                            extType     = sad->extLength >> 30;
                            extPosition = sad->extPosition;

                        } else {
                            dwarn("LONG #%d\n", si);
                            long_ad *lad = (long_ad *)(ADArray + si*descLen);
                            extLength   = lad->extLength & 0x3FFFFFFF;
                            extType     = lad->extLength >> 30;
                            extPosition = lad->extLocation.logicalBlockNum;
                        }

                        dbg("ExtLen: %u, type: %u, ExtLoc: %u\n", extLength, extType, extPosition);
                        dbg("usedSpace: %u\n", get_used_blocks(&stats->found));

                        if (extType < 2) {
                            // Allocated
                            increment_used_space(stats, extLength, extPosition);
                        }
                        uint32_t lbSize = (uint32_t) stats->blocksize;
                        lsn = lsn + (extLength / lbSize);
                        dbg("LSN: %u, ExtLocOrig: %u\n", lsn, extPosition);
                        dbg("usedSpace: %u\n", get_used_blocks(&stats->found));
                        dwarn("Size: %u, Blocks: %u\n", extLength, extLength / lbSize);
                    }

                    free(ADArray);
                }
            } else if(icbTagADFlags == ICBTAG_FLAG_AD_EXTENDED) {
                if(dir) {
                    fid_inspected = 1;
                    walk_directory(media, lsn, allocDescs, L_AD,
                                   ICBTAG_FLAG_AD_EXTENDED, stats, depth, seq, &status);
                } else {
                    err("EAD found. Please report.\n");
                }
            } else if(icbTagADFlags == ICBTAG_FLAG_AD_IN_ICB) {
                dbg("AD in ICB\n");
            } else {
                dbg("ICB TAG->flags: 0x%02x\n", fe->icbTag.flags);
            }

            // We can assume that directory have one or more FID inside.
            // FE have inside long_ad/short_ad.
            if(dir && fid_inspected == 0) {
                uint8_t *dirContent;
                uint32_t lengthAllocDescs;
                if(ext) {
                    dbg("[EFE DIR] lengthExtendedAttr: %u\n", efe->lengthExtendedAttr);
                    dbg("[EFE DIR] lengthAllocDescs: %u\n", efe->lengthAllocDescs);
                    dirContent = efe->extendedAttrAndAllocDescs + efe->lengthExtendedAttr;
                    lengthAllocDescs = efe->lengthAllocDescs;
                } else {
                    dbg("[FE DIR] lengthExtendedAttr: %u\n", fe->lengthExtendedAttr);
                    dbg("[FE DIR] lengthAllocDescs: %u\n", fe->lengthAllocDescs);
                    dirContent = fe->extendedAttrAndAllocDescs + fe->lengthExtendedAttr;
                    lengthAllocDescs = fe->lengthAllocDescs;
                }

                uint8_t tempStatus = 0;
                for(uint32_t pos=0; pos < lengthAllocDescs; ) {
                    uint8_t failureCode = inspect_fid(media, lsn,
                                                      dirContent, &pos, stats, depth+1, seq,
                                                      &tempStatus);
                    if(failureCode) {
                        dbg("1 FID inspection over.\n");
                        break;
                    }
                }
                dbg("2 FID inspection over.\n");
                if (tempStatus & ESTATUS_CORRECTED_ERRORS) {
                    // FID(s) were fixed - update FE/EFE CRC
                    descTag = &efe->descTag;  // same as &fe->descTag
                    descTag->descCRC = udf_crc((uint8_t *)(descTag + 1),  descTag->descCRCLength, 0);
                    descTag->tagChecksum = calculate_checksum(*descTag);
                }
                status |= tempStatus;
            }
            break;  
        default:
            err("IDENT: %x, LSN: %u, addr: 0x%" PRIx64 "\n", descTag->tagIdent, lsn,
                lsn * stats->blocksize);
    }            
    // unmap_chunk(dev, chunk, devsize); 
    return status;
}

/**
 * \brief File tree entry point
 *
 * This function is entry for file tree parsing. It actually parses two trees, Stream file tree based on Stream Directory ICB and normal File tree based on Root Directory ICB.
 *
 * \param[in]      media     Information regarding medium & access to it
 * \pararm[in,out] *stats    file system status
 * \param[in]      *seq      VDS sequence
 *
 * \return sum of returns from stream and normal get_file()
 */
uint8_t get_file_structure(udf_media_t *media,
                           struct filesystemStats *stats, vds_sequence_t *seq ) {
    uint32_t lsn, slsn;

    int status = 0;
    uint32_t elen = 0, selen = 0;

    int vds = -1;
    if((vds=get_correct(seq, TAG_IDENT_LVD)) < 0) {
        err("No correct LVD found. Aborting.\n");
        return ESTATUS_UNCORRECTED_ERRORS;
    }
    dbg("VDS used: %d\n", vds);
#ifdef MEMTRACE
    dbg("Disc ptr: %p, LVD ptr: %p\n", &media->disc, media->disc.udf_lvd[vds]);
    dbg("Disc ptr: %p, FSD ptr: %p\n", &media->disc, media->disc.udf_fsd);
#endif

    // Go to ROOT ICB 
    lb_addr icbloc = media->disc.udf_fsd->rootDirectoryICB.extLocation;
    // Get Stream Dir ICB
    lb_addr sicbloc = media->disc.udf_fsd->streamDirectoryICB.extLocation;
    dbg("icbloc: %u\n", icbloc.logicalBlockNum);
    dbg("sicbloc: %u\n", sicbloc.logicalBlockNum);

    lsn   = icbloc.logicalBlockNum  + stats->lbnlsn;
    slsn  = sicbloc.logicalBlockNum + stats->lbnlsn;
    elen  = media->disc.udf_fsd->rootDirectoryICB.extLength;
    selen = media->disc.udf_fsd->streamDirectoryICB.extLength;
    dbg("ROOT LSN: %u, len: %u, partition: %u\n", lsn, elen, icbloc.partitionReferenceNum);
    dbg("STREAM LSN: %u len: %u, partition: %u\n", slsn, selen, sicbloc.partitionReferenceNum);

    dbg("Used space offset: %u\n", get_used_blocks(&stats->found));
    struct fileInfo info;
    memset(&info, 0, sizeof(struct fileInfo));

    if(selen > 0) {
        msg("\nStream file tree\n----------------\n");
        status |= get_file(media, slsn, stats, 0, 0, info, seq);
    }
    if(elen > 0) {
        msg("\nMedium file tree\n----------------\n");
        status |= get_file(media, lsn, stats, 0, 0, info, seq);
    }
    return status;
}

/**
 * \brief Support function for appending error to seq structure
 *
 * \param[in,out] seq VDS sequence
 * \param[in] tagIdent identifier of descriptor to append
 * \param[in] vds VDS to search
 * \param[in] error to append
 *
 * \return 0 everything OK
 * \return -1 required descriptor not found
 */
int append_error(vds_sequence_t *seq, uint16_t tagIdent, vds_type_e vds, uint8_t error) {
    for(int i=0; i<VDS_STRUCT_AMOUNT; ++i) {
        if(vds == MAIN_VDS) {
            if(seq->main[i].tagIdent == tagIdent) {
                seq->main[i].error |= error;
                return 0;
            }
        } else {
            if(seq->reserve[i].tagIdent == tagIdent) {
                seq->reserve[i].error |= error;
                return 0;
            }
        }
    }
    return -1;
}

/**
 * \brief Support function for getting error from seq structure
 *
 * \param[in,out] *seq VDS sequence
 * \param[in] tagIdent identifier of descriptor to find
 * \param[in] vds VDS to search
 *
 * \return requested error if found or UINT8_MAX if not 
 */
uint8_t get_error(vds_sequence_t *seq, uint16_t tagIdent, vds_type_e vds) {
    for(int i=0; i<VDS_STRUCT_AMOUNT; ++i) {
        if(vds == MAIN_VDS) {
            if(seq->main[i].tagIdent == tagIdent) {
                return seq->main[i].error;
            }
        } else {
            if(seq->reserve[i].tagIdent == tagIdent) {
                return seq->reserve[i].error;
            }
        }
    }
    return -1;
}

/**
 * \brief Support function for getting tag location from seq structure
 *
 * \param[in,out] *seq VDS sequence
 * \param[in] tagIdent identifier of descriptor to find
 * \param[in] vds VDS to search
 *
 * \return requested location if found or UINT32_MAX if not 
 */
uint32_t get_tag_location(vds_sequence_t *seq, uint16_t tagIdent, vds_type_e vds) {
    for(int i=0; i<VDS_STRUCT_AMOUNT; ++i) {
        if(vds == MAIN_VDS) {
            if(seq->main[i].tagIdent == tagIdent) {
                return seq->main[i].tagLocation;
            }
        } else {
            if(seq->reserve[i].tagIdent == tagIdent) {
                return seq->reserve[i].tagLocation;
            }
        }
    }
    return -1;
}


/**
 * \brief VDS verification structure
 *
 * This function go thru all VDS descriptors and check them for checksum, CRC and position.
 * Results are stored using append_error() function.
 *
 * \param[in] *disc UDF disc structure
 * \param[in] vds VDS to search
 * \param[in,out] *seq VDS sequence for error storing
 *
 * \return 0
 */
int verify_vds(struct udf_disc *disc, vds_type_e vds, vds_sequence_t *seq, struct filesystemStats *stats) {
    if(!checksum(disc->udf_pvd[vds]->descTag)) {
        err("Checksum failure at PVD[%d]\n", vds);
        append_error(seq, TAG_IDENT_PVD, vds, E_CHECKSUM);
    }   
    if(!checksum(disc->udf_lvd[vds]->descTag)) {
        err("Checksum failure at LVD[%d]\n", vds);
        append_error(seq, TAG_IDENT_LVD, vds, E_CHECKSUM);
    }   
    if(!checksum(disc->udf_pd[vds]->descTag)) {
        err("Checksum failure at PD[%d]\n", vds);
        append_error(seq, TAG_IDENT_PD, vds, E_CHECKSUM);
    }   
    if(!checksum(disc->udf_usd[vds]->descTag)) {
        err("Checksum failure at USD[%d]\n", vds);
        append_error(seq, TAG_IDENT_USD, vds, E_CHECKSUM);
    }   
    if(!checksum(disc->udf_iuvd[vds]->descTag)) {
        err("Checksum failure at IUVD[%d]\n", vds);
        append_error(seq, TAG_IDENT_IUVD, vds, E_CHECKSUM);
    }   
    if(!checksum(disc->udf_td[vds]->descTag)) {
        err("Checksum failure at TD[%d]\n", vds);
        append_error(seq, TAG_IDENT_TD, vds, E_CHECKSUM);
    }

    if(check_position(disc->udf_pvd[vds]->descTag, get_tag_location(seq, TAG_IDENT_PVD, vds))) {
        err("Position failure at PVD[%d]\n", vds);
        append_error(seq, TAG_IDENT_PVD, vds, E_POSITION);
    }   
    if(check_position(disc->udf_lvd[vds]->descTag, get_tag_location(seq, TAG_IDENT_LVD, vds))) {
        err("Position failure at LVD[%d]\n", vds);
        append_error(seq, TAG_IDENT_LVD, vds, E_POSITION);
    }   
    if(check_position(disc->udf_pd[vds]->descTag, get_tag_location(seq, TAG_IDENT_PD, vds))) {
        err("Position failure at PD[%d]\n", vds);
        append_error(seq, TAG_IDENT_PD, vds, E_POSITION);
    }   
    if(check_position(disc->udf_usd[vds]->descTag, get_tag_location(seq, TAG_IDENT_USD, vds))) {
        err("Position failure at USD[%d]\n", vds);
        append_error(seq, TAG_IDENT_USD, vds, E_POSITION);
    }   
    if(check_position(disc->udf_iuvd[vds]->descTag, get_tag_location(seq, TAG_IDENT_IUVD, vds))) {
        err("Position failure at IUVD[%d]\n", vds);
        append_error(seq, TAG_IDENT_IUVD, vds, E_POSITION);
    }   
    if(check_position(disc->udf_td[vds]->descTag, get_tag_location(seq, TAG_IDENT_TD, vds))) {
        err("Position failure at TD[%d]\n", vds);
        append_error(seq, TAG_IDENT_TD, vds, E_POSITION);
    }

    if(crc(disc->udf_pvd[vds], sizeof(struct primaryVolDesc))) {
        err("CRC error at PVD[%d]\n", vds);
        append_error(seq, TAG_IDENT_PVD, vds, E_CRC);
    }
    if(crc(disc->udf_lvd[vds], sizeof(struct logicalVolDesc)+disc->udf_lvd[vds]->mapTableLength)) {
        err("CRC error at LVD[%d]\n", vds);
        append_error(seq, TAG_IDENT_LVD, vds, E_CRC);
    }
    if(crc(disc->udf_pd[vds], sizeof(struct partitionDesc))) {
        err("CRC error at PD[%d]\n", vds);
        append_error(seq, TAG_IDENT_PD, vds, E_CRC);
    }
    if(crc(disc->udf_usd[vds], sizeof(struct unallocSpaceDesc)+(disc->udf_usd[vds]->numAllocDescs)*sizeof(extent_ad))) {
        err("CRC error at USD[%d]\n", vds);
        append_error(seq, TAG_IDENT_USD, vds, E_CRC);
    }
    if(crc(disc->udf_iuvd[vds], sizeof(struct impUseVolDesc))) {
        err("CRC error at IUVD[%d]\n", vds);
        append_error(seq, TAG_IDENT_IUVD, vds, E_CRC);
    }
    if(crc(disc->udf_td[vds], sizeof(struct terminatingDesc))) {
        err("CRC error at TD[%d]\n", vds);
        append_error(seq, TAG_IDENT_TD, vds, E_CRC);
    }


    if(get_error(seq, TAG_IDENT_LVD, vds) == 0) {
        stats->dstringLVDLogicalVolIdentErr[vds] = check_dstring(disc->udf_lvd[vds]->logicalVolIdent, 128);
    }

    if(get_error(seq, TAG_IDENT_PVD, vds) == 0) {
        stats->dstringPVDVolIdentErr[vds] = check_dstring(disc->udf_pvd[vds]->volIdent, 32);
        stats->dstringPVDVolSetIdentErr[vds] = check_dstring(disc->udf_pvd[vds]->volSetIdent, 128);
    }

    if(get_error(seq, TAG_IDENT_IUVD, vds) == 0) {
        struct impUseVolDescImpUse * impUse = (struct impUseVolDescImpUse *)disc->udf_iuvd[vds]->impUse;

        stats->dstringIUVDLVInfo1Err[vds] = check_dstring(impUse->LVInfo1, 36);
        stats->dstringIUVDLVInfo2Err[vds] = check_dstring(impUse->LVInfo2, 36);
        stats->dstringIUVDLVInfo3Err[vds] = check_dstring(impUse->LVInfo3, 36);
        stats->dstringIUVDLogicalVolIdentErr[vds] = check_dstring(impUse->logicalVolIdent, 128);
    }

    dbg("Verify VDS done\n");
    return 0;
}

/**
 * \brief Copy descriptor from one position to another on medium
 *
 * Also fixes declared position, CRC and checksum of the new copy.
 *
 * \param[in]  media                Information regarding medium & access to it
 * \param[in]  sourcePosition       in blocks
 * \param[in]  destinationPosition  in blocks
 * \param[in]  size                 size of descriptor to copy
 *
 * return 0
 */
int copy_descriptor(udf_media_t *media, uint32_t sourcePosition, uint32_t destinationPosition,
                    size_t size) {
    tag sourceDescTag, destinationDescTag;
    uint8_t *destArray;
    uint32_t offset = 0, chunk = 0;
    uint32_t chunksize = CHUNK_SIZE;
    uint64_t byte_position;

    dbg("source: 0x%x, destination: 0x%x\n", sourcePosition, destinationPosition);

    byte_position = sourcePosition * (uint64_t)media->sectorsize;
    chunk  = (uint32_t)(byte_position / chunksize);
    offset = (uint32_t)(byte_position % chunksize);
    dbg("Chunk: %u, offset: 0x%x\n", chunk, offset);
    map_chunk(media, chunk, __FILE__, __LINE__);

    sourceDescTag = *(tag *)(media->mapping[chunk] + offset);
    memcpy(&destinationDescTag, &sourceDescTag, sizeof(tag));
    destinationDescTag.tagLocation = destinationPosition;
    destinationDescTag.tagChecksum = calculate_checksum(destinationDescTag);

    dbg("srcChecksum: 0x%x, destChecksum: 0x%x\n", sourceDescTag.tagChecksum, destinationDescTag.tagChecksum);

    destArray = calloc(1, size);
    memcpy(destArray, &destinationDescTag, sizeof(tag));
    memcpy(destArray+sizeof(tag), media->mapping[chunk] + offset + sizeof(tag), size - sizeof(tag));

    unmap_chunk(media, chunk);
    byte_position = destinationPosition * (uint64_t)media->sectorsize;
    chunk  = (uint32_t)(byte_position / chunksize);
    offset = (uint32_t)(byte_position % chunksize);
    dbg("Chunk: %u, offset: 0x%x\n", chunk, offset);
    map_chunk(media, chunk, __FILE__, __LINE__);

    memcpy(media->mapping[chunk] + offset, destArray, size);

    free(destArray);

    unmap_chunk(media, chunk);

    return 0;
}

/**
 * \brief Writes back specified AVDP from udf_disc structure to device
 *
 * \param[in] media    Information regarding medium & access to it
 * \param[in] source   source AVDP
 * \param[in] target   target AVDP
 *
 * \return 0 everything OK
 * \return -2 after write checksum failed 
 * \return -4 after write CRC failed
 */
int write_avdp(udf_media_t *media, avdp_type_e source, avdp_type_e target) {
    uint64_t sourcePosition = 0;
    uint64_t targetPosition = 0;
    tag desc_tag;
    avdp_type_e type = target;
    uint32_t offset = 0, chunk = 0;
    uint32_t chunksize = CHUNK_SIZE;

    // Source type determines position on media
    if(source == 0) {
        sourcePosition = media->sectorsize * 256;            // First AVDP is on LSN=256
    } else if(source == 1) {
        sourcePosition = media->devsize - media->sectorsize; // Second AVDP is on last LSN
    } else if(source == 2) {
    	// Third AVDP can be at last LSN - 256
        sourcePosition = media->devsize - media->sectorsize - 256 * media->sectorsize;
    } else {
        sourcePosition = media->sectorsize * 512; // Unclosed disc can have AVDP at sector 512
    }

    // Target type determines position on media
    if(target == 0) {
        targetPosition = media->sectorsize * 256;            // First AVDP is on LSN=256
    } else if(target == 1) {
        targetPosition = media->devsize - media->sectorsize; // Second AVDP is on last LSN
    } else if(target == 2) {
    	// Third AVDP can be at last LSN - 256
        targetPosition = media->devsize - media->sectorsize - 256 * media->sectorsize;
    } else {
        targetPosition = media->sectorsize * 512; // Unclosed disc can have AVDP at sector 512
        type = FIRST_AVDP; //Save it to FIRST_AVDP positon
    }

    dbg("DevSize: %" PRIu64 "\n", media->devsize);
    dbg("Current position: %" PRIx64 "\n", targetPosition);

    copy_descriptor(media,
                    sourcePosition / media->sectorsize,
                    targetPosition / media->sectorsize,
                    sizeof(struct anchorVolDescPtr));

    free(media->disc.udf_anchor[type]);
    media->disc.udf_anchor[type] = malloc(sizeof(struct anchorVolDescPtr)); // Prepare memory for AVDP

    chunk = targetPosition/chunksize;
    offset = targetPosition%chunksize;
    dbg("Chunk: %u, offset: 0x%x\n", chunk, offset);
    map_chunk(media, chunk, __FILE__, __LINE__);

    desc_tag = *(tag *)(media->mapping[chunk] + offset);

    if(!checksum(desc_tag)) {
        err("Checksum failure at AVDP[%d]\n", type);
        map_chunk(media, chunk, __FILE__, __LINE__);
        return -2;
    } else if(le16_to_cpu(desc_tag.tagIdent) != TAG_IDENT_AVDP) {
        err("AVDP not found at 0x%" PRIx64 "\n", targetPosition);
        map_chunk(media, chunk, __FILE__, __LINE__);
        return -4;
    }

    memcpy(media->disc.udf_anchor[type], media->mapping[chunk] + offset, sizeof(struct anchorVolDescPtr));

    if (crc(media->disc.udf_anchor[type], sizeof(struct anchorVolDescPtr))) {
        err("CRC error at AVDP[%d]\n", type);
        map_chunk(media, chunk, __FILE__, __LINE__);
        return -3;
    }

    imp("AVDP[%d] successfully written.\n", type);
    map_chunk(media, chunk, __FILE__, __LINE__);
    return 0;
}

/**
 * \brief Fix target AVDP's extent length
 *
 * \param[in] media    Information regarding medium & access to it
 * \param[in] target   target AVDP
 *
 * \return 0 everything OK
 * \return -2 checksum failed 
 * \return -4 CRC failed
 */
int fix_avdp(udf_media_t *media, avdp_type_e target) {
    uint64_t targetPosition = 0;
    tag desc_tag;
    avdp_type_e type = target;
    uint32_t offset = 0, chunk = 0;
    uint32_t chunksize = CHUNK_SIZE;

    // Target type determines position on media
    if(target == 0) {
        targetPosition = media->sectorsize * 256;            // First AVDP is at LSN=256
    } else if(target == 1) {
        targetPosition = media->devsize - media->sectorsize; // Second AVDP is at last LSN
    } else if(target == 2) {
        // Third AVDP can be at last LSN - 256
        targetPosition = media->devsize - media->sectorsize - 256 * media->sectorsize;
    } else {
        targetPosition = media->sectorsize * 512; // Unclosed disc can have AVDP at sector 512
        type = FIRST_AVDP; //Save it to FIRST_AVDP position
    }

    dbg("DevSize: %" PRIu64 "\n", media->devsize);
    dbg("Current position: %" PRIx64 "\n", targetPosition);

    chunk = targetPosition/chunksize;
    offset = targetPosition%chunksize;
    dbg("Chunk: %u, offset: 0x%x\n", chunk, offset);
    map_chunk(media, chunk, __FILE__, __LINE__);

    desc_tag = *(tag *)(media->mapping[chunk] + offset);

    if(!checksum(desc_tag)) {
        err("Checksum failure at AVDP[%d]\n", type);
        return -2;
    } else if(le16_to_cpu(desc_tag.tagIdent) != TAG_IDENT_AVDP) {
        err("AVDP not found at 0x%" PRIx64 "\n", targetPosition);
        return -4;
    }

    if(  media->disc.udf_anchor[type]->mainVolDescSeqExt.extLength
       > media->disc.udf_anchor[type]->reserveVolDescSeqExt.extLength) { //main is bigger
        if (   media->disc.udf_anchor[type]->mainVolDescSeqExt.extLength
            >= 16 * media->sectorsize) { //and is big enough
            media->disc.udf_anchor[type]->reserveVolDescSeqExt.extLength = media->disc.udf_anchor[type]->mainVolDescSeqExt.extLength;
        } 
    } else { //reserve is bigger
        if (   media->disc.udf_anchor[type]->reserveVolDescSeqExt.extLength
            >= 16 * media->sectorsize) { //and is big enough
            media->disc.udf_anchor[type]->mainVolDescSeqExt.extLength = media->disc.udf_anchor[type]->reserveVolDescSeqExt.extLength;
        } 
    }
    media->disc.udf_anchor[type]->descTag.descCRC = calculate_crc(media->disc.udf_anchor[type], sizeof(struct anchorVolDescPtr));
    media->disc.udf_anchor[type]->descTag.tagChecksum = calculate_checksum(media->disc.udf_anchor[type]->descTag);

    memcpy(media->mapping[chunk] + offset, media->disc.udf_anchor[type], sizeof(struct anchorVolDescPtr));

    imp("AVDP[%d] Extent Length successfully fixed.\n", type);
    return 0;
}

/**
 * \brief Get descriptor name string
 *
 * \param[in] descIdent descriptor identifier
 * 
 * \return constant char array
 */
char * descriptor_name(uint16_t descIdent) {
    switch(descIdent) {
        case TAG_IDENT_PVD:
            return "PVD";
        case TAG_IDENT_LVD:
            return "LVD";
        case TAG_IDENT_PD:
            return "PD";
        case TAG_IDENT_USD:
            return "USD";
        case TAG_IDENT_IUVD:
            return "IUVD";
        case TAG_IDENT_TD:
            return "TD";
        case TAG_IDENT_AVDP:
            return "AVDP";
        case TAG_IDENT_LVID:
            return "LVID";
        default:
            return "Unknown";
    }
}

/**
 * \brief Fix VDS sequence 
 *
 * This function checks the VDS; if an error is found, the second VDS is checked.
 * If secondary is ok, copy it, if not, report unrecoverable error.
 *
 * \param[in]      media     Information regarding medium & access to it
 * \param[in]      source    AVDP pointing to VDS
 * \param[in,out]  *seq      VDS sequence
 *
 * \return sum of 0, 1 and 4 according fixing and found errors
 */
int fix_vds(udf_media_t *media, avdp_type_e source, vds_sequence_t *seq) {
    uint32_t position_main, position_reserve;
    uint8_t fix=0;
    uint8_t status = 0;

    // Go to first address of VDS
    position_main    = media->disc.udf_anchor[source]->mainVolDescSeqExt.extLocation;
    position_reserve = media->disc.udf_anchor[source]->reserveVolDescSeqExt.extLocation;


    msg("\nVDS verification status\n-----------------------\n");

    for(int i=0; i<VDS_STRUCT_AMOUNT; ++i) {
        if(seq->main[i].error != 0 && seq->reserve[i].error != 0) {
            //Both descriptors are broken
            //TODO It can be possible to reconstruct some descriptors, but not all. 
            err("[%d] Both descriptors are broken. May not be able to continue later.\n",i);
        } else if(seq->main[i].error != 0) {
            //Copy Reserve -> Main
            if(interactive) {
                fix = prompt("%s is broken. Fix it? [Y/n]", descriptor_name(seq->reserve[i].tagIdent)); 
            } else if (autofix) {
                fix = 1;
            }

            //int copy_descriptor(uint8_t *dev, struct udf_disc *disc, size_t sectorsize, uint32_t sourcePosition, uint32_t destinationPosition, size_t amount);
            if(fix) {
                warn("[%d] Fixing Main %s\n",i,descriptor_name(seq->reserve[i].tagIdent));
                warn("sectorsize: %zu\n", media->sectorsize);
                warn("src pos: 0x%x\n", position_reserve + i);
                warn("dest pos: 0x%x\n", position_main + i);
                //                memcpy(position_main + i*sectorsize, position_reserve + i*sectorsize, sectorsize);
                copy_descriptor(media, position_reserve + i, position_main + i, media->sectorsize);
                status |= ESTATUS_CORRECTED_ERRORS;
            } else {
                err("[%i] %s is broken.\n", i,descriptor_name(seq->reserve[i].tagIdent));
                status |= ESTATUS_UNCORRECTED_ERRORS;
            }
            fix = 0;
        } else if(seq->reserve[i].error != 0) {
            //Copy Main -> Reserve
            if(interactive) {
                fix = prompt("%s is broken. Fix it? [Y/n]", descriptor_name(seq->main[i].tagIdent)); 
            } else if (autofix) {
                fix = 1;
            }

            if(fix) {
                warn("[%i] Fixing Reserve %s\n", i,descriptor_name(seq->main[i].tagIdent));
                copy_descriptor(media, position_reserve + i, position_main + i, media->sectorsize);
                status |= ESTATUS_CORRECTED_ERRORS;
            } else {
                err("[%i] %s is broken.\n", i,descriptor_name(seq->main[i].tagIdent));
                status |= ESTATUS_UNCORRECTED_ERRORS;
            }
            fix = 0;
        } else {
            msg("[%d] %s is fine. No functional fixing needed.\n", i, descriptor_name(seq->main[i].tagIdent));
        }
        if(seq->main[i].tagIdent == TAG_IDENT_TD)
            break;
    }


    return status;
}

static const unsigned char BitsSetTable256[256] = 
{
#define B2(n) n,     n+1,     n+1,     n+2
#define B4(n) B2(n), B2(n+1), B2(n+1), B2(n+2)
#define B6(n) B4(n), B4(n+1), B4(n+1), B4(n+2)
    B6(0), B6(1), B6(1), B6(2)
}; ///< Support array for bit counting

/**
 * \brief Fix PD Partition Header contents
 *
 * At this moment it only fixes SBD, because no other space descriptors are supported.
 *
 * \param[in]      media     Information regarding medium & access to it
 * \param[in,out]  *stats    file system status
 * \param[in]      *seq      VDS sequence
 *
 * \return 0 -- All OK
 * \return 1 -- PD SBD recovery failed
 * \return 4 -- No correct PD found
 * \return -1 -- no SBD found even if declared
 */
int fix_pd(udf_media_t *media, struct filesystemStats *stats, vds_sequence_t *seq) {
    int vds = -1;
    uint32_t chunksize = CHUNK_SIZE;
    uint32_t chunk = 0;
    uint32_t offset = 0;

    if((vds=get_correct(seq, TAG_IDENT_PD)) < 0) {
        err("No correct PD found. Aborting.\n");
        return 4;
    }
    struct partitionHeaderDesc *phd = (struct partitionHeaderDesc *)(media->disc.udf_pd[vds]->partitionContentsUse);
    dbg("[USD] UST pos: %u, len: %u\n", phd->unallocSpaceTable.extPosition, phd->unallocSpaceTable.extLength);
    dbg("[USD] USB pos: %u, len: %u\n", phd->unallocSpaceBitmap.extPosition, phd->unallocSpaceBitmap.extLength);
    dbg("[USD] FST pos: %u, len: %u\n", phd->freedSpaceTable.extPosition, phd->freedSpaceTable.extLength);
    dbg("[USD] FSB pos: %u, len: %u\n", phd->freedSpaceBitmap.extPosition, phd->freedSpaceBitmap.extLength);

    if(phd->unallocSpaceTable.extLength > 0) {
        //Unhandled. Not found on any medium.
        err("[USD] Unallocated Space Table is unhandled. Skipping.\n");
    }
    if(phd->freedSpaceTable.extLength > 0) {
        //Unhandled. Not found on any medium.
        err("[USD] Free Space Table is unhandled. Skipping.\n");
    }
    if(phd->freedSpaceBitmap.extLength > 0) {
        //Unhandled. Not found on any medium.
        err("[USD] Unallocated Space Table is unhandled. Skipping.\n");
    }

    if(phd->unallocSpaceBitmap.extLength > 3) { //0,1,2,3 are special values ECMA 167r3 4/14.14.1.1
        uint32_t lbnlsn   = media->disc.udf_pd[vds]->partitionStartingLocation;
        uint64_t position = (lbnlsn + phd->unallocSpaceBitmap.extPosition) * stats->blocksize;

        chunk  = (uint32_t) (position / chunksize);
        offset = (uint32_t) (position % chunksize);
        map_chunk(media, chunk, __FILE__, __LINE__);

        struct spaceBitmapDesc *sbd = (struct spaceBitmapDesc *)(media->mapping[chunk] + offset);
        if(sbd->descTag.tagIdent != TAG_IDENT_SBD) {
            err("SBD not found\n");
            return -1;
        }
        dbg("[SBD] NumOfBits: %u\n", sbd->numOfBits);
        dbg("[SBD] NumOfBytes: %u\n", sbd->numOfBytes);
        dbg("[SBD] Chunk: %u, Offset: %u\n", chunk, offset);

#ifdef MEMTRACE    
        dbg("Bitmap: %u, %p\n", lbnlsn + phd->unallocSpaceBitmap.extPosition, sbd->bitmap);
#else
        dbg("Bitmap: %u\n", lbnlsn + phd->unallocSpaceBitmap.extPosition);
#endif
        memcpy(sbd->bitmap, stats->actPartitionBitmap, sbd->numOfBytes);
        dbg("MEMCPY DONE\n");

        //Recalculate CRC and checksum
        sbd->descTag.descCRC = calculate_crc(sbd, sbd->descTag.descCRCLength + sizeof(tag));
        sbd->descTag.tagChecksum = calculate_checksum(sbd->descTag);
        
        imp("PD SBD recovery was successful.\n");
        return 0;
    }
    err("PD SBD recovery failed.\n");
    return 1;
}

/**
 * \brief Get PD Partition Header contents
 *
 * At this moment it handles SBD only, because other space descriptors are not supported.
 *
 * \param[in]  media   Information regarding medium & access to it
 * \param[out] *stats  filesystem status
 * \param[in] *seq     VDS sequence
 *
 * \return 0 -- All OK
 * \return 4 --No correct PD found
 * \return -1 -- SBD not found even if declared
 * \return -128 -- UST, FST or FSB found
 */
int get_pd(udf_media_t *media, struct filesystemStats *stats, vds_sequence_t *seq) {
    int vds = -1;
    uint32_t offset = 0, chunk = 0;
    uint32_t chunksize = CHUNK_SIZE;
    uint64_t position = 0;

    if((vds=get_correct(seq, TAG_IDENT_PD)) < 0) {
        err("No correct PD found. Aborting.\n");
        return 4;
    }

    stats->partitionAccessType      = media->disc.udf_pd[vds]->accessType;
    stats->found.partitionNumBlocks = media->disc.udf_pd[vds]->partitionLength;
    stats->found.freeSpaceBlocks    = media->disc.udf_pd[vds]->partitionLength;

    // Create array for used/unused blocks counting
    uint32_t bitmapNumBytes = (stats->found.partitionNumBlocks + 7) / 8;
    stats->actPartitionBitmap  = malloc(bitmapNumBytes);
    memset(stats->actPartitionBitmap, 0xff, bitmapNumBytes);
    dbg("Create array done\n");

    struct partitionHeaderDesc *phd = (struct partitionHeaderDesc *)(media->disc.udf_pd[vds]->partitionContentsUse);
    dbg("[USD] UST pos: %u, len: %u\n", phd->unallocSpaceTable.extPosition, phd->unallocSpaceTable.extLength);
    dbg("[USD] USB pos: %u, len: %u\n", phd->unallocSpaceBitmap.extPosition, phd->unallocSpaceBitmap.extLength);
    dbg("[USD] FST pos: %u, len: %u\n", phd->freedSpaceTable.extPosition, phd->freedSpaceTable.extLength);
    dbg("[USD] FSB pos: %u, len: %u\n", phd->freedSpaceBitmap.extPosition, phd->freedSpaceBitmap.extLength);

    if(phd->unallocSpaceTable.extLength > 0) {
        //Unhandled. Not found on any medium.
        err("[USD] Unallocated Space Table is unhandled. Skipping.\n");
        return -128;
    }
    if(phd->freedSpaceTable.extLength > 0) {
        //Unhandled. Not found on any medium.
        err("[USD] Free Space Table is unhandled. Skipping.\n");
        return -128;
    }
    if(phd->freedSpaceBitmap.extLength > 0) {
        //Unhandled. Not found on any medium.
        err("[USD] Freed Space Bitmap is unhandled. Skipping.\n");
        return -128;
    }
    if(phd->unallocSpaceBitmap.extLength > 3) { //0,1,2,3 are special values ECMA 167r3 4/14.14.1.1
        uint32_t lbnlsn = media->disc.udf_pd[vds]->partitionStartingLocation;
        dbg("LBN 0: LSN %u\n", lbnlsn);
        position = (lbnlsn + phd->unallocSpaceBitmap.extPosition) * stats->blocksize;
        chunk  = (uint32_t)(position / chunksize);
        offset = (uint32_t)(position % chunksize);
        map_chunk(media, chunk, __FILE__, __LINE__);

        struct spaceBitmapDesc *sbd = (struct spaceBitmapDesc *)(media->mapping[chunk] + offset);
        if(sbd->descTag.tagIdent != TAG_IDENT_SBD) {
            err("SBD not found\n");
            return -1;
        }
        if(!checksum(sbd->descTag)) {
            err("SBD checksum error. Continue with caution.\n");
            seq->pd.error |= E_CHECKSUM;
        }
        if(crc(sbd, sbd->descTag.descCRCLength + sizeof(tag))) {
            err("SBD CRC error. Continue with caution.\n");
            seq->pd.error |= E_CRC; 
        }
        if (sbd->numOfBits != stats->found.partitionNumBlocks) {
            err("SBD size error. Continue with caution.\n");
            seq->pd.error |= E_FREESPACE;
        }
        dbg("SBD is ok\n");
        dbg("[SBD] NumOfBits: %u\n", sbd->numOfBits);
        dbg("[SBD] NumOfBytes: %u\n", sbd->numOfBytes);
#ifdef MEMTRACE
        dbg("Bitmap: %u, %p\n", lbnlsn + phd->unallocSpaceBitmap.extPosition, sbd->bitmap);
#else
        dbg("Bitmap: %u\n", lbnlsn + phd->unallocSpaceBitmap.extPosition);
#endif

        stats->spacedesc.partitionNumBlocks = sbd->numOfBits;

        uint8_t *ptr = NULL;
        dbg("Chunk: %u\n", chunk);
        map_raw(media->fd, &ptr, (uint64_t)(chunk) * CHUNK_SIZE, (sbd->numOfBytes + offset), media->devsize);
#ifdef MEMTRACE
        dbg("Ptr: %p\n", ptr); 
#endif
        sbd = (struct spaceBitmapDesc *)(ptr+offset);
       
        dbg("Get bitmap statistics\n"); 
        //Get actual bitmap statistics
        uint32_t unusedBlocks = 0;
        uint8_t count = 0;
        uint8_t v = 0;
        for(int i=0; i<(int)(sbd->numOfBytes-1); i++) {
            v = sbd->bitmap[i];
            //if(i%1000 == 0) dbg("0x%02x %d\n",v, i);
            count = BitsSetTable256[v & 0xff] + BitsSetTable256[(v >> 8) & 0xff] + BitsSetTable256[(v >> 16) & 0xff] + BitsSetTable256[v >> 24];     
            unusedBlocks += count;
        }
        dbg("Unused blocks: %u\n", unusedBlocks);

        uint8_t bitCorrection = sbd->numOfBytes*8-sbd->numOfBits;
        dbg("BitCorrection: %u\n", bitCorrection);
        v = sbd->bitmap[sbd->numOfBytes-1];
        dbg("Bitmap last: 0x%02x\n", v);
        for(int i=0; i<8 - bitCorrection; i++) {
            dbg("Mask: 0x%02x, Result: 0x%02x\n", (1 << i), v & (1 << i));
            if(v & (1 << i))
                unusedBlocks++;
        }

        stats->spacedesc.freeSpaceBlocks = unusedBlocks;
        stats->expPartitionBitmap = sbd->bitmap;
        dbg("Unused blocks: %u\n", unusedBlocks);
        dbg("Used Blocks: %u\n", get_used_blocks(&stats->spacedesc));

        sbd = (struct spaceBitmapDesc *)(media->mapping[chunk] + offset);
        unmap_raw(&ptr, (uint64_t)(chunk)*CHUNK_SIZE, sbd->numOfBytes);
        unmap_chunk(media, chunk);
    }

    //Mark used space
    increment_used_space(stats, phd->unallocSpaceTable.extLength, phd->unallocSpaceTable.extPosition);
    increment_used_space(stats, phd->unallocSpaceBitmap.extLength, phd->unallocSpaceBitmap.extPosition);
    increment_used_space(stats, phd->freedSpaceTable.extLength, phd->freedSpaceTable.extPosition);
    increment_used_space(stats, phd->freedSpaceBitmap.extLength, phd->freedSpaceBitmap.extPosition);

    return 0; 
}

/**
 * \brief Fix LVID values
 *
 * This function rebuilds the LVID from scratch if the recorded one is damaged and not just out-of-date.
 *
 * \param[in]  media   Information regarding medium & access to it
 * \param[in] *stats   filesystem status
 * \param[in] *seq     VDS sequence
 *
 * \return 0 -- All Ok
 * \return 4 -- No correct LVD found
 */
int fix_lvid(udf_media_t *media, struct filesystemStats *stats, vds_sequence_t *seq) {
    int vds = -1;
    uint32_t chunksize = CHUNK_SIZE;
    uint32_t chunk = 0;
    uint32_t offset = 0;
    uint64_t position;

    if((vds=get_correct(seq, TAG_IDENT_LVD)) < 0) {
        err("No correct LVD found. Aborting.\n");
        return 4;
    }

    uint32_t loc = media->disc.udf_lvd[vds]->integritySeqExt.extLocation;
    uint32_t len = media->disc.udf_lvd[vds]->integritySeqExt.extLength;

    position = loc * stats->blocksize;
    chunk  = (uint32_t)(position / chunksize);
    offset = (uint32_t)(position % chunksize);
    map_chunk(media, chunk, __FILE__, __LINE__);

    struct logicalVolIntegrityDesc *lvid = (struct logicalVolIntegrityDesc *)(media->mapping[chunk] + offset);

    // Fix PD too
    fix_pd(media, stats, seq);

    // These two may not be correct if LVID is damaged
    uint16_t size =   sizeof(struct logicalVolIntegrityDesc)
                    + media->disc.udf_lvid->numOfPartitions * sizeof(uint32_t) * 2
                    + media->disc.udf_lvid->lengthOfImpUse;
    struct impUseLVID *impUse = (struct impUseLVID *)(  (uint8_t *)(media->disc.udf_lvid)
                                                      + size
                                                      - media->disc.udf_lvid->lengthOfImpUse);

    if (seq->lvid.error & (E_CRC | E_CHECKSUM | E_WRONGDESC))
    {
        // A full rebuild of the LVID is needed
        size =   sizeof(struct logicalVolIntegrityDesc)
               + sizeof(uint32_t) * 2  // Partition size and free block count (1 partition)
               + sizeof(struct logicalVolIntegrityDescImpUse);

        impUse = (struct impUseLVID *)(  (uint8_t *)(media->disc.udf_lvid)
                                       + size
                                       - sizeof(struct logicalVolIntegrityDescImpUse));

        memset(media->disc.udf_lvid, 0, size);    // @todo blank the entire integrity extent?

        media->disc.udf_lvid->descTag.tagIdent      = constant_cpu_to_le16(TAG_IDENT_LVID);
        media->disc.udf_lvid->descTag.descVersion   = (stats->found.minUDFReadRev < 0x0200)
                                                        ? constant_cpu_to_le16(2)
                                                        : constant_cpu_to_le16(3);
        media->disc.udf_lvid->descTag.descCRCLength = cpu_to_le16(size - sizeof(tag));
        media->disc.udf_lvid->descTag.tagSerialNum  = constant_cpu_to_le16(1);
        media->disc.udf_lvid->descTag.tagLocation   = cpu_to_le32(loc);
        media->disc.udf_lvid->numOfPartitions       = constant_cpu_to_le32(1);
        media->disc.udf_lvid->lengthOfImpUse        = constant_cpu_to_le32(sizeof(struct logicalVolIntegrityDescImpUse));

        impUse->minUDFReadRev  = cpu_to_le16(stats->found.minUDFReadRev);
        impUse->minUDFWriteRev = cpu_to_le16(stats->found.minUDFWriteRev);
        impUse->maxUDFWriteRev = cpu_to_le16(stats->found.maxUDFWriteRev);

        strcpy(impUse->impID.ident, UDF_ID_DEVELOPER);
        impUse->impID.identSuffix[0] = UDF_OS_CLASS_UNIX;
        impUse->impID.identSuffix[1] = UDF_OS_ID_LINUX;
    }
    dbg("LVID: loc: %u, len: %u, size: %u\n", loc, len, size);

    // Fix file/dir counts
    impUse->numOfFiles = cpu_to_le32(stats->found.numFiles);
    impUse->numOfDirs  = cpu_to_le32(stats->found.numDirs);

    // Fix Next Unique ID
    //((struct logicalVolHeaderDesc *)(disc->udf_lvid->logicalVolContentsUse))->uniqueID = stats->maxUUID+1;
    struct logicalVolHeaderDesc *lvhd = (struct logicalVolHeaderDesc *)(media->disc.udf_lvid->logicalVolContentsUse);

    lvhd->uniqueID = cpu_to_le64(stats->found.nextUID);

    // Set recording date and time to now. 
    time_t t = time(NULL);
    struct tm tmlocal = *localtime(&t);
    struct tm tm = *gmtime(&t);
    int8_t hrso = tmlocal.tm_hour - tm.tm_hour;
    if(hrso > 12 || hrso < -12) {
        hrso += 24;
    }

    int8_t mino = tmlocal.tm_min - tm.tm_min;
    int16_t t_offset = hrso*60+mino;
    dbg("Offset: %d, hrs: %d, min: %d\n", t_offset, hrso, mino);
    dbg("lhr: %d, hr: %d\n", tmlocal.tm_hour, tm.tm_hour);

    timestamp *ts = &(media->disc.udf_lvid->recordingDateAndTime);
    ts->typeAndTimezone =   constant_cpu_to_le16(1 << 12)
                          | cpu_to_le16(t_offset >= 0 ? t_offset : (0x1000-t_offset));
    ts->year  = cpu_to_le16(tmlocal.tm_year + 1900);
    ts->month = tmlocal.tm_mon + 1;
    ts->day = tmlocal.tm_mday;
    ts->hour = tmlocal.tm_hour;
    ts->minute = tmlocal.tm_min;
    ts->second = tmlocal.tm_sec;
    ts->centiseconds = 0;
    ts->hundredsOfMicroseconds = 0;
    ts->microseconds = 0;
    dbg("Type and Timezone: 0x%04x\n", le16_to_cpu(ts->typeAndTimezone));

    uint32_t *freeSpaceTable = (uint32_t *) media->disc.udf_lvid->data;
    uint32_t *sizeTable      = freeSpaceTable + media->disc.udf_lvid->numOfPartitions;

    sizeTable[0]      = cpu_to_le32(stats->found.partitionNumBlocks);
    freeSpaceTable[0] = cpu_to_le32(stats->found.freeSpaceBlocks);
    dbg("New Free Space: %u\n", stats->found.freeSpaceBlocks);

    // Close integrity (last thing before write)
    media->disc.udf_lvid->integrityType = constant_cpu_to_le32(LVID_INTEGRITY_TYPE_CLOSE);

    // Recalculate CRC and checksum
    media->disc.udf_lvid->descTag.descCRC = calculate_crc(media->disc.udf_lvid, size);
    media->disc.udf_lvid->descTag.tagChecksum = calculate_checksum(media->disc.udf_lvid->descTag);
    //Write changes back to medium
    memcpy(lvid, media->disc.udf_lvid, size);

    unmap_chunk(media, chunk);
    imp("LVID recovery was successful.\n");
    return 0;
}

