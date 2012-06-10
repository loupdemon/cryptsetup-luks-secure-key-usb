/*
 * dm-verity volume handling
 *
 * Copyright (C) 2012, Red Hat, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <uuid/uuid.h>

#include "libcryptsetup.h"
#include "verity.h"
#include "internal.h"

#define VERITY_SIGNATURE "verity\0\0"

#define NEW_SB 1

#ifndef NEW_SB
struct verity_sb {
	uint8_t signature[8];
	uint8_t version;
	uint8_t data_block_bits;
	uint8_t hash_block_bits;
	uint8_t pad1[1];
	uint16_t salt_size;
	uint8_t pad2[2];
	uint32_t data_blocks_hi;
	uint32_t data_blocks_lo;
	uint8_t algorithm[16];
	uint8_t salt[384];
	uint8_t pad3[88];
};
#else
struct verity_sb {
	uint8_t  signature[8];	/* "verity\0\0" */
	uint32_t version;	/* superblock version */
	uint32_t hash_type;	/* 0 - Chrome OS, 1 - normal */
	uint8_t  uuid[16];	/* UUID of hash device */
	uint8_t  algorithm[32];/* hash algorithm name */
	uint64_t data_block_size; /* data block in bytes */
	uint64_t hash_block_size; /* hash block in bytes */
	uint64_t data_blocks;	/* number of data blocks */
	uint64_t salt_size;	/* salt size */
	uint8_t  salt[256];	/* salt */
	uint8_t  _pad[160];
} __attribute__((packed));
#endif

/* Read verity superblock from disk */
int VERITY_read_sb(struct crypt_device *cd,
		   const char *device,
		   uint64_t sb_offset,
		   char **uuid_string,
		   struct crypt_params_verity *params)
{
	struct verity_sb sb = {};
	ssize_t hdr_size = sizeof(struct verity_sb);
	int devfd = 0, sb_version;
#ifndef NEW_SB
	uint64_t sb_data_blocks;
#endif
	log_dbg("Reading VERITY header of size %u on device %s, offset %" PRIu64 ".",
		sizeof(struct verity_sb), device, sb_offset);

	if (params->flags & CRYPT_VERITY_NO_HEADER) {
		log_err(cd, _("Verity device doesn't use on-disk header.\n"), device);
		return -EINVAL;
	}

	devfd = open(device ,O_RDONLY | O_DIRECT);
	if(devfd == -1) {
		log_err(cd, _("Cannot open device %s.\n"), device);
		return -EINVAL;
	}

	if(lseek(devfd, sb_offset, SEEK_SET) < 0 ||
	   read_blockwise(devfd, &sb, hdr_size) < hdr_size) {
		close(devfd);
		return -EIO;
	}
	close(devfd);

	if (memcmp(sb.signature, VERITY_SIGNATURE, sizeof(sb.signature))) {
		log_err(cd, _("Device %s is not a valid VERITY device.\n"), device);
		return -EINVAL;
	}
#ifndef NEW_SB
	if (sb.version > 1) {
		log_err(cd, _("Unsupported VERITY version %d.\n"), sb.version);
		return -EINVAL;
	}

	if (sb.data_block_bits < 9 || sb.data_block_bits >= 31 ||
	    sb.hash_block_bits < 9 || sb.hash_block_bits >= 31 ||
	    !memchr(sb.algorithm, 0, sizeof(sb.algorithm)) ||
	    ntohs(sb.salt_size) > 256) {
		log_err(cd, _("VERITY header corrupted.\n"));
		return -EINVAL;
	}

	sb_data_blocks = ((uint64_t)ntohl(sb.data_blocks_hi) << 31 << 1) |
				ntohl(sb.data_blocks_lo);

	params->hash_name = strdup((const char*)sb.algorithm);
	if (!params->hash_name)
		return -ENOMEM;
	params->data_block_size = 1 << sb.data_block_bits;
	params->hash_block_size = 1 << sb.hash_block_bits;
	params->data_size = sb_data_blocks;
	params->salt_size = ntohs(sb.salt_size);
	params->salt = malloc(params->salt_size);
	if (!params->salt)
		return -ENOMEM;
	memcpy(CONST_CAST(char*)params->salt, sb.salt, params->salt_size);
	params->hash_type = sb.version;
#else
	sb_version = le32_to_cpu(sb.version);
	if (sb_version != 1) {
		log_err(cd, _("Unsupported VERITY version %d.\n"), sb_version);
		return -EINVAL;
	}
	params->hash_type = le32_to_cpu(sb.hash_type);
	if (params->hash_type > 1) {
		log_err(cd, _("Unsupported VERITY hash type %d.\n"), params->hash_type);
		return -EINVAL;
	}

	params->data_block_size = le64_to_cpu(sb.data_block_size);
	params->hash_block_size = le64_to_cpu(sb.hash_block_size);
	if (params->data_block_size % 512 || params->hash_block_size % 512) {
		log_err(cd, _("Unsupported VERITY block size.\n"));
		return -EINVAL;
	}
	params->data_size = le64_to_cpu(sb.data_blocks);

	params->hash_name = strndup((const char*)sb.algorithm, sizeof(sb.algorithm));
	if (!params->hash_name)
		return -ENOMEM;

	params->salt_size = le64_to_cpu(sb.salt_size);
	if (params->salt_size > sizeof(sb.salt)) {
		log_err(cd, _("VERITY header corrupted.\n"));
		free(CONST_CAST(char*)params->hash_name);
		return -EINVAL;
	}
	params->salt = malloc(params->salt_size);
	if (!params->salt) {
		free(CONST_CAST(char*)params->hash_name);
		return -ENOMEM;
	}
	memcpy(CONST_CAST(char*)params->salt, sb.salt, params->salt_size);

	if ((*uuid_string = malloc(40)))
		uuid_unparse(sb.uuid, *uuid_string);
#endif
	params->hash_area_offset = sb_offset;
	return 0;
}

/* Write verity superblock to disk */
int VERITY_write_sb(struct crypt_device *cd,
		   const char *device,
		   uint64_t sb_offset,
		   const char *uuid_string,
		   struct crypt_params_verity *params)
{
	struct verity_sb sb = {};
	ssize_t hdr_size = sizeof(struct verity_sb);
	uuid_t uuid;
	int r, devfd = 0;

	log_dbg("Updating VERITY header of size %u on device %s, offset %" PRIu64 ".",
		sizeof(struct verity_sb), device, sb_offset);

	if (!uuid_string || uuid_parse(uuid_string, uuid) == -1) {
		log_err(cd, _("Wrong VERITY UUID format provided.\n"), device);
		return -EINVAL;
	}

	if (params->flags & CRYPT_VERITY_NO_HEADER) {
		log_err(cd, _("Verity device doesn't use on-disk header.\n"), device);
		return -EINVAL;
	}

	devfd = open(device, O_RDWR | O_DIRECT);
	if(devfd == -1) {
		log_err(cd, _("Cannot open device %s.\n"), device);
		return -EINVAL;
	}

	memcpy(&sb.signature, VERITY_SIGNATURE, sizeof(sb.signature));
#ifndef NEW_SB
	sb.version = params->hash_type;
	sb.data_block_bits = ffs(params->data_block_size) - 1;
	sb.hash_block_bits = ffs(params->hash_block_size) - 1;
	sb.salt_size = htons(params->salt_size);
	sb.data_blocks_hi = htonl(params->data_size >> 31 >> 1);
	sb.data_blocks_lo = htonl(params->data_size & 0xFFFFFFFF);
	strncpy((char *)sb.algorithm, params->hash_name, sizeof(sb.algorithm));
	memcpy(sb.salt, params->salt, params->salt_size);
#else
	sb.version         = cpu_to_le32(1);
	sb.hash_type       = cpu_to_le32(params->hash_type);
	sb.data_block_size = cpu_to_le64(params->data_block_size);
	sb.hash_block_size = cpu_to_le64(params->hash_block_size);
	sb.salt_size       = cpu_to_le64(params->salt_size);
	sb.data_blocks     = cpu_to_le64(params->data_size);
	strncpy((char *)sb.algorithm, params->hash_name, sizeof(sb.algorithm));
	memcpy(sb.salt, params->salt, params->salt_size);
	memcpy(sb.uuid, uuid, sizeof(sb.uuid));
#endif
	r = write_lseek_blockwise(devfd, (char*)&sb, hdr_size, sb_offset) < hdr_size ? -EIO : 0;
	if (r)
		log_err(cd, _("Error during update of verity header on device %s.\n"), device);
	close(devfd);

	return r;
}

/* Calculate hash offset in hash blocks */
uint64_t VERITY_hash_offset_block(struct crypt_params_verity *params)
{
	uint64_t hash_offset = params->hash_area_offset;

	if (params->flags & CRYPT_VERITY_NO_HEADER)
		return hash_offset / params->hash_block_size;

	hash_offset += sizeof(struct verity_sb);
	hash_offset += params->hash_block_size - 1;

	return hash_offset / params->hash_block_size;
}

int VERITY_UUID_generate(struct crypt_device *cd, char **uuid_string)
{
	uuid_t uuid;

	if (!(*uuid_string = malloc(40)))
		return -ENOMEM;
	uuid_generate(uuid);
	uuid_unparse(uuid, *uuid_string);
	return 0;
}

/* Activate verity device in kernel device-mapper */
int VERITY_activate(struct crypt_device *cd,
		     const char *name,
		     const char *hash_device,
		     const char *root_hash,
		     size_t root_hash_size,
		     struct crypt_params_verity *verity_hdr,
		     uint32_t activation_flags)
{
	struct crypt_dm_active_device dmd;
	uint64_t offset = 0;
	int r;

	log_dbg("Trying to activate VERITY device %s using hash %s.",
		name ?: "[none]", verity_hdr->hash_name);

	if (verity_hdr->flags & CRYPT_VERITY_CHECK_HASH) {
		r = VERITY_verify(cd, verity_hdr,
				  crypt_get_device_name(cd), hash_device,
				  root_hash, root_hash_size);
		if (r < 0)
			return r;
	}

	if (!name)
		return 0;

	dmd.target = DM_VERITY;
	dmd.data_device = crypt_get_device_name(cd);
	dmd.u.verity.hash_device = hash_device;
	dmd.u.verity.root_hash = root_hash;
	dmd.u.verity.root_hash_size = root_hash_size;
	dmd.u.verity.hash_offset = VERITY_hash_offset_block(verity_hdr),
	dmd.flags = activation_flags;
	dmd.size = verity_hdr->data_size * verity_hdr->data_block_size / 512;
	dmd.uuid = crypt_get_uuid(cd);
	dmd.u.verity.vp = verity_hdr;

	r = device_check_and_adjust(cd, dmd.data_device, DEV_EXCL,
				    &dmd.size, &offset, &dmd.flags);
	if (r)
		return r;

	r = dm_create_device(name, CRYPT_VERITY, &dmd, 0);
	if (!r && !(dm_flags() & DM_VERITY_SUPPORTED)) {
		log_err(cd, _("Kernel doesn't support dm-verity mapping.\n"));
		return -ENOTSUP;
	}
	if (r < 0)
		return r;

	r = dm_status_verity_ok(name);
	if (r < 0)
		return r;

	if (!r)
		log_err(cd, _("Verity device detected corruption after activation.\n"));
	return 0;
}
