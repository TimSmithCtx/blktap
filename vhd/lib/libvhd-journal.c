/* Copyright (c) 2008, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 */
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "atomicio.h"
#include "libvhd-journal.h"

#define VHD_JOURNAL_ENTRY_TYPE_FOOTER    0
#define VHD_JOURNAL_ENTRY_TYPE_HEADER    1
#define VHD_JOURNAL_ENTRY_TYPE_LOCATOR   2
#define VHD_JOURNAL_ENTRY_TYPE_BAT       3
#define VHD_JOURNAL_ENTRY_TYPE_BATMAP_H  4
#define VHD_JOURNAL_ENTRY_TYPE_BATMAP_M  5
#define VHD_JOURNAL_ENTRY_TYPE_DATA      6

typedef struct vhd_journal_entry {
	uint32_t                         type;
	uint32_t                         size;
	uint64_t                         offset;
	uint64_t                         cookie;
	uint32_t                         checksum;
} vhd_journal_entry_t;

static inline int
vhd_journal_seek(vhd_journal_t *j, off64_t offset, int whence)
{
	off64_t off;

	off = lseek64(j->jfd, offset, whence);
	if (off == (off64_t)-1)
		return -errno;

	return 0;
}

static inline off64_t
vhd_journal_position(vhd_journal_t *j)
{
	return lseek64(j->jfd, 0, SEEK_CUR);
}

static inline int
vhd_journal_read(vhd_journal_t *j, void *buf, size_t size)
{
	ssize_t ret;

	errno = 0;

	ret = atomicio(read, j->jfd, buf, size);
	if (ret != size)
		return (errno ? -errno : -EIO);

	return 0;
}

static inline int
vhd_journal_write(vhd_journal_t *j, void *buf, size_t size)
{
	ssize_t ret;

	errno = 0;

	ret = atomicio(vwrite, j->jfd, buf, size);
	if (ret != size)
		return (errno ? -errno : -EIO);

	return 0;
}

static inline int
vhd_journal_truncate(vhd_journal_t *j, off64_t length)
{
	int err;

	err = ftruncate(j->jfd, length);
	if (err == -1)
		return -errno;

	return 0;
}

static inline void
vhd_journal_header_in(vhd_journal_header_t *header)
{
	BE32_IN(&header->entries);
	BE64_IN(&header->footer_offset);
}

static inline void
vhd_journal_header_out(vhd_journal_header_t *header)
{
	BE32_OUT(&header->entries);
	BE64_OUT(&header->footer_offset);
}

static int
vhd_journal_validate_header(vhd_journal_t *j, vhd_journal_header_t *header)
{
	int err;

	if (memcmp(header->cookie,
		   VHD_JOURNAL_HEADER_COOKIE, sizeof(header->cookie)))
		return -EINVAL;

	return 0;
}

static int
vhd_journal_read_journal_header(vhd_journal_t *j, vhd_journal_header_t *header)
{
	int err;
	size_t size;

	size = sizeof(vhd_journal_header_t);
	err  = vhd_journal_seek(j, 0, SEEK_SET);
	if (err)
		return err;

	err  = vhd_journal_read(j, header, size);
	if (err)
		return err;

	vhd_journal_header_in(header);

	return vhd_journal_validate_header(j, header);
}

static int
vhd_journal_write_header(vhd_journal_t *j, vhd_journal_header_t *header)
{
	int err;
	size_t size;
	vhd_journal_header_t h;

	memcpy(&h, header, sizeof(vhd_journal_header_t));

	size = sizeof(vhd_journal_header_t);
	err  = vhd_journal_seek(j, 0, SEEK_SET);
	if (err)
		return err;

	err = vhd_journal_validate_header(j, &h);
	if (err)
		return err;

	vhd_journal_header_out(&h);

	err = vhd_journal_write(j, &h, size);
	if (err)
		return err;

	return 0;
}

static int
vhd_journal_add_journal_header(vhd_journal_t *j)
{
	int err;
	off64_t off;
	vhd_context_t *vhd;

	vhd = &j->vhd;
	memset(&j->header, 0, sizeof(vhd_journal_header_t));

	err = vhd_seek(vhd, 0, SEEK_END);
	if (err)
		return err;

	off = vhd_position(vhd);
	if (off == (off64_t)-1)
		return -errno;

	err = vhd_get_footer(vhd);
	if (err)
		return err;

	uuid_copy(j->header.uuid, vhd->footer.uuid);
	memcpy(j->header.cookie,
	       VHD_JOURNAL_HEADER_COOKIE, sizeof(j->header.cookie));
	j->header.footer_offset = off - sizeof(vhd_footer_t);

	return vhd_journal_write_header(j, &j->header);
}

static void
vhd_journal_entry_in(vhd_journal_entry_t *entry)
{
	BE32_IN(&entry->type);
	BE32_IN(&entry->size);
	BE64_IN(&entry->offset);
	BE64_IN(&entry->cookie);
	BE32_IN(&entry->checksum);
}

static void
vhd_journal_entry_out(vhd_journal_entry_t *entry)
{
	BE32_OUT(&entry->type);
	BE32_OUT(&entry->size);
	BE64_OUT(&entry->offset);
	BE64_OUT(&entry->cookie);
	BE32_OUT(&entry->checksum);
}

static uint32_t
vhd_journal_checksum_entry(vhd_journal_entry_t *entry, char *buf, size_t size)
{
	int i;
	unsigned char *blob;
	uint32_t checksum, tmp;

	checksum        = 0;
	tmp             = entry->checksum;
	entry->checksum = 0;

	blob = (unsigned char *)entry;
	for (i = 0; i < sizeof(vhd_journal_entry_t); i++)
		checksum += blob[i];

	blob = (unsigned char *)buf;
	for (i = 0; i < size; i++)
		checksum += blob[i];

	entry->checksum = tmp;
	return ~checksum;
}

static int
vhd_journal_validate_entry(vhd_journal_entry_t *entry)
{
	if (entry->size == 0)
		return -EINVAL;

	if (entry->size & (VHD_SECTOR_SIZE - 1))
		return -EINVAL;

	if (entry->cookie != VHD_JOURNAL_ENTRY_COOKIE)
		return -EINVAL;

	return 0;
}

static int
vhd_journal_read_entry(vhd_journal_t *j, vhd_journal_entry_t *entry)
{
	int err;

	err = vhd_journal_read(j, entry, sizeof(vhd_journal_entry_t));
	if (err)
		return err;

	vhd_journal_entry_in(entry);
	return vhd_journal_validate_entry(entry);
}

static int
vhd_journal_write_entry(vhd_journal_t *j, vhd_journal_entry_t *entry)
{
	int err;
	vhd_journal_entry_t e;

	err = vhd_journal_validate_entry(entry);
	if (err)
		return err;

	memcpy(&e, entry, sizeof(vhd_journal_entry_t));
	vhd_journal_entry_out(&e);

	err = vhd_journal_write(j, &e, sizeof(vhd_journal_entry_t));
	if (err)
		err;

	return 0;
}

static int
vhd_journal_validate_entry_data(vhd_journal_entry_t *entry, char *buf)
{
	int err;
	uint32_t checksum;

	err      = 0;
	checksum = vhd_journal_checksum_entry(entry, buf, entry->size);

	if (checksum != entry->checksum)
		return -EINVAL;

	return err;
}

static int
vhd_journal_update(vhd_journal_t *j, off64_t offset,
		   char *buf, size_t size, uint32_t type)
{
	int err;
	off64_t eof;
	vhd_journal_entry_t entry;

	entry.type     = type;
	entry.size     = size;
	entry.offset   = offset;
	entry.cookie   = VHD_JOURNAL_ENTRY_COOKIE;
	entry.checksum = vhd_journal_checksum_entry(&entry, buf, size);

	err = vhd_journal_seek(j, 0, SEEK_END);
	if (err)
		return err;

	eof = vhd_journal_position(j);
	if (eof == (off64_t)-1)
		return -errno;

	err = vhd_journal_write_entry(j, &entry);
	if (err)
		goto fail;

	err = vhd_journal_write(j, buf, size);
	if (err)
		goto fail;

	j->header.entries++;
	err = vhd_journal_write_header(j, &j->header);
	if (err) {
		j->header.entries--;
		goto fail;
	}

	return 0;

fail:
	vhd_journal_truncate(j, eof);
	return err;
}

static int
vhd_journal_add_footer(vhd_journal_t *j)
{
	int err;
	off64_t off;
	vhd_context_t *vhd;
	vhd_footer_t footer;

	vhd = &j->vhd;

	err = vhd_read_footer(vhd, &footer);
	if (err)
		return err;

	err = vhd_seek(vhd, 0, SEEK_END);
	if (err)
		return err;

	off = vhd_position(vhd);
	if (off == (off64_t)-1)
		return -errno;

	vhd_footer_out(&footer);
	err = vhd_journal_update(j, off - sizeof(vhd_footer_t),
				 (char *)&footer,
				 sizeof(vhd_footer_t),
				 VHD_JOURNAL_ENTRY_TYPE_FOOTER);

	return err;
}

static int
vhd_journal_add_header(vhd_journal_t *j)
{
	int err;
	off64_t off;
	vhd_context_t *vhd;
	vhd_header_t header;

	vhd = &j->vhd;

	err = vhd_read_header(vhd, &header);
	if (err)
		return err;

	off = vhd->footer.data_offset;

	vhd_header_out(&header);
	err = vhd_journal_update(j, off,
				 (char *)&header,
				 sizeof(vhd_header_t),
				 VHD_JOURNAL_ENTRY_TYPE_HEADER);

	return err;
}

static int
vhd_journal_add_locators(vhd_journal_t *j)
{
	int i, n, err;
	vhd_context_t *vhd;

	vhd = &j->vhd;

	err = vhd_get_header(vhd);
	if (err)
		return err;

	n = sizeof(vhd->header.loc) / sizeof(vhd_parent_locator_t);
	for (i = 0; i < n; i++) {
		char *buf;
		off64_t off;
		size_t size;
		vhd_parent_locator_t *loc;

		loc  = vhd->header.loc + i;
		err  = vhd_validate_platform_code(loc->code);
		if (err)
			return err;

		if (loc->code == PLAT_CODE_NONE)
			continue;

		off  = loc->data_offset;
		size = vhd_parent_locator_size(loc);

		err  = posix_memalign((void **)&buf, VHD_SECTOR_SIZE, size);
		if (err)
			return -err;

		err  = vhd_seek(vhd, off, SEEK_SET);
		if (err)
			goto end;

		err  = vhd_read(vhd, buf, size);
		if (err)
			goto end;

		err  = vhd_journal_update(j, off, buf, size,
					  VHD_JOURNAL_ENTRY_TYPE_LOCATOR);
		if (err)
			goto end;

		err = 0;

	end:
		free(buf);
		if (err)
			break;
	}

	return err;
}

static int
vhd_journal_add_bat(vhd_journal_t *j)
{
	int err;
	off64_t off;
	vhd_bat_t bat;
	size_t size, secs;
	vhd_context_t *vhd;

	vhd  = &j->vhd;

	err  = vhd_get_header(vhd);
	if (err)
		return err;

	err  = vhd_read_bat(vhd, &bat);
	if (err)
		return err;

	off  = vhd->header.table_offset;
	secs = secs_round_up_no_zero(bat.entries * sizeof(uint32_t));
	size = secs << VHD_SECTOR_SHIFT;

	vhd_bat_out(&bat);
	err  = vhd_journal_update(j, off, (char *)bat.bat, size,
				  VHD_JOURNAL_ENTRY_TYPE_BAT);

	free(bat.bat);
	return err;
}

static int
vhd_journal_add_batmap(vhd_journal_t *j)
{
	int err;
	off64_t off;
	size_t size, secs;
	vhd_context_t *vhd;
	vhd_batmap_t batmap;

	vhd  = &j->vhd;

	err  = vhd_batmap_header_offset(vhd, &off);
	if (err)
		return err;

	err  = vhd_read_batmap(vhd, &batmap);
	if (err)
		return err;

	secs = secs_round_up_no_zero(sizeof(struct dd_batmap_hdr));
	size = secs << VHD_SECTOR_SHIFT;

	vhd_batmap_header_out(&batmap);
	err  = vhd_journal_update(j, off, (char *)&batmap.header, size,
				  VHD_JOURNAL_ENTRY_TYPE_BATMAP_H);
	if (err)
		goto out;

	vhd_batmap_header_in(&batmap);
	off  = batmap.header.batmap_offset;
	secs = batmap.header.batmap_size;
	size = secs << VHD_SECTOR_SHIFT;

	err  = vhd_journal_update(j, off, batmap.map, size,
				  VHD_JOURNAL_ENTRY_TYPE_BATMAP_M);

out:
	free(batmap.map);
	return err;
}

static int
vhd_journal_add_metadata(vhd_journal_t *j)
{
	int err;
	char *buf;
	size_t size;
	off64_t off, eof;
	vhd_context_t *vhd;

	vhd = &j->vhd;

	err = vhd_journal_add_footer(j);
	if (err)
		return err;

	if (!vhd_type_dynamic(vhd))
		return 0;

	err = vhd_journal_add_header(j);
	if (err)
		return err;

	err = vhd_journal_add_locators(j);
	if (err)
		return err;

	err = vhd_journal_add_bat(j);
	if (err)
		return err;

	if (vhd_has_batmap(vhd)) {
		err = vhd_journal_add_batmap(j);
		if (err)
			return err;
	}

	return 0;
}

static int
vhd_journal_read_footer(vhd_journal_t *j, vhd_footer_t *footer)
{
	int err;
	vhd_journal_entry_t entry;

	err = vhd_journal_read_entry(j, &entry);
	if (err)
		return err;

	if (entry.type != VHD_JOURNAL_ENTRY_TYPE_FOOTER)
		return -EINVAL;

	if (entry.size != sizeof(vhd_footer_t))
		return -EINVAL;

	err = vhd_journal_read(j, footer, entry.size);
	if (err)
		return err;

	vhd_footer_in(footer);
	return vhd_validate_footer(footer);
}

static int
vhd_journal_read_header(vhd_journal_t *j, vhd_header_t *header)
{
	int err;
	vhd_journal_entry_t entry;

	err = vhd_journal_read_entry(j, &entry);
	if (err)
		return err;

	if (entry.type != VHD_JOURNAL_ENTRY_TYPE_HEADER)
		return -EINVAL;

	if (entry.size != sizeof(vhd_header_t))
		return -EINVAL;

	err = vhd_journal_read(j, header, entry.size);
	if (err)
		return err;

	vhd_header_in(header);
	return vhd_validate_header(header);
}

static int
vhd_journal_read_locators(vhd_journal_t *j, char ***locators, int *locs)
{
	int err, n, _locs;
	char **_locators, *buf;
	vhd_journal_entry_t entry;

	_locs     = 0;
	*locs     = 0;
	*locators = NULL;

	n = sizeof(j->vhd.header.loc) / sizeof(vhd_parent_locator_t);
	_locators = calloc(n, sizeof(char *));
	if (!_locators)
		return -ENOMEM;

	for (;;) {
		buf = NULL;

		err = vhd_journal_read_entry(j, &entry);
		if (err)
			goto fail;

		if (entry.type != VHD_JOURNAL_ENTRY_TYPE_LOCATOR)
			break;

		if (_locs >= n) {
			err = -EINVAL;
			goto fail;
		}

		err = posix_memalign((void **)&buf,
				     VHD_SECTOR_SIZE, entry.size);
		if (err) {
			err = -err;
			buf = NULL;
			goto fail;
		}

		err = vhd_journal_read(j, buf, entry.size);
		if (err)
			goto fail;

		_locators[_locs++] = buf;
		err                = 0;
	}


	*locs     = _locs;
	*locators = _locators;

	return 0;

fail:
	if (_locators) {
		for (n = 0; n < _locs; n++)
			free(_locators[n]);
		free(_locators);
	}
	return err;
}

static int
vhd_journal_read_bat(vhd_journal_t *j, vhd_bat_t *bat)
{
	int err;
	size_t size, secs;
	vhd_context_t *vhd;
	vhd_journal_entry_t entry;

	vhd  = &j->vhd;

	secs = secs_round_up_no_zero(vhd->header.max_bat_size *
				     sizeof(uint32_t));
	size = secs << VHD_SECTOR_SHIFT;

	err  = vhd_journal_read_entry(j, &entry);
	if (err)
		return err;

	if (entry.type != VHD_JOURNAL_ENTRY_TYPE_BAT)
		return -EINVAL;

	if (entry.size != size)
		return -EINVAL;

	if (entry.offset != vhd->header.table_offset)
		return -EINVAL;

	err = posix_memalign((void **)&bat->bat, VHD_SECTOR_SIZE, size);
	if (err)
		return -err;

	err = vhd_journal_read(j, bat->bat, entry.size);
	if (err)
		goto fail;

	bat->spb     = vhd->header.block_size << VHD_SECTOR_SHIFT;
	bat->entries = vhd->header.max_bat_size;
	vhd_bat_in(bat);

	return 0;

fail:
	free(bat->bat);
	bat->bat = NULL;
	return err;
}

static int
vhd_journal_read_batmap_header(vhd_journal_t *j, vhd_batmap_t *batmap)
{
	int err;
	size_t size, secs;
	vhd_journal_entry_t entry;

	secs = secs_round_up_no_zero(sizeof(struct dd_batmap_hdr));
	size = secs << VHD_SECTOR_SHIFT;

	err  = vhd_journal_read_entry(j, &entry);
	if (err)
		return err;

	if (entry.type != VHD_JOURNAL_ENTRY_TYPE_BATMAP_H)
		return -EINVAL;

	if (entry.size != size)
		return -EINVAL;

	err = vhd_journal_read(j, &batmap->header, entry.size);
	if (err)
		return err;

	vhd_batmap_header_in(batmap);
	return vhd_validate_batmap_header(batmap);
}

static int
vhd_journal_read_batmap_map(vhd_journal_t *j, vhd_batmap_t *batmap)
{
	int err;
	vhd_journal_entry_t entry;

	err  = vhd_journal_read_entry(j, &entry);
	if (err)
		return err;

	if (entry.type != VHD_JOURNAL_ENTRY_TYPE_BATMAP_M)
		return -EINVAL;

	if (entry.size != batmap->header.batmap_size << VHD_SECTOR_SHIFT)
		return -EINVAL;

	if (entry.offset != batmap->header.batmap_offset)
		return -EINVAL;

	err = posix_memalign((void **)&batmap->map,
			     VHD_SECTOR_SIZE, entry.size);
	if (err)
		return -err;

	err = vhd_journal_read(j, batmap->map, entry.size);
	if (err) {
		free(batmap->map);
		batmap->map = NULL;
		return err;
	}

	return 0;
}

static int
vhd_journal_read_batmap(vhd_journal_t *j, vhd_batmap_t *batmap)
{
	int err;

	err = vhd_journal_read_batmap_header(j, batmap);
	if (err)
		return err;

	err = vhd_journal_read_batmap_map(j, batmap);
	if (err)
		return err;

	err = vhd_validate_batmap(batmap);
	if (err) {
		free(batmap->map);
		batmap->map = NULL;
		return err;
	}

	return 0;
}

static int
vhd_journal_restore_footer(vhd_journal_t *j, vhd_footer_t *footer)
{
	int err;
	vhd_context_t *vhd;

	vhd = &j->vhd;

	err = vhd_write_footer_at(vhd, footer, j->header.footer_offset);
	if (err)
		return err;

	if (!vhd_type_dynamic(vhd))
		return 0;

	return vhd_write_footer_at(vhd, footer, 0);
}

static int
vhd_journal_restore_header(vhd_journal_t *j, vhd_header_t *header)
{
	off64_t off;
	vhd_context_t *vhd;

	vhd = &j->vhd;
	off = vhd->footer.data_offset;

	return vhd_write_header_at(&j->vhd, header, off);
}

static int
vhd_journal_restore_locators(vhd_journal_t *j, char **locators, int locs)
{
	size_t size;
	vhd_context_t *vhd;
	int i, n, lidx, err;
	vhd_parent_locator_t *loc;

	lidx = 0;
	vhd  = &j->vhd;

	n = sizeof(vhd->header.loc) / sizeof(vhd_parent_locator_t);

	for (i = 0; i < n && lidx < locs; i++) {
		loc  = vhd->header.loc + i;
		if (loc->code == PLAT_CODE_NONE)
			continue;

		err  = vhd_seek(vhd, loc->data_offset, SEEK_SET);
		if (err)
			return err;

		size = vhd_parent_locator_size(loc);
		err  = vhd_write(vhd, locators[lidx++], size);
		if (err)
			return err;
	}

	return 0;
}

static int
vhd_journal_restore_bat(vhd_journal_t *j, vhd_bat_t *bat)
{
	return vhd_write_bat(&j->vhd, bat);
}

static int
vhd_journal_restore_batmap(vhd_journal_t *j, vhd_batmap_t *batmap)
{
	return vhd_write_batmap(&j->vhd, batmap);
}

static int
vhd_journal_restore_metadata(vhd_journal_t *j)
{
	char **locators;
	vhd_context_t *vhd;
	int i, locs, hlocs, err;

	vhd      = &j->vhd;
	locs     = 0;
	hlocs    = 0;
	locators = NULL;

	err = vhd_journal_seek(j, sizeof(vhd_journal_header_t), SEEK_SET);
	if (err)
		return err;

	err  = vhd_journal_read_footer(j, &vhd->footer);
	if (err)
		return err;

	if (!vhd_type_dynamic(vhd))
		goto restore;
	
	err  = vhd_journal_read_header(j, &vhd->header);
	if (err)
		return err;

	for (hlocs = 0, i = 0; i < vhd_parent_locator_count(vhd); i++) {
		if (vhd_validate_platform_code(vhd->header.loc[i].code))
			return err;

		if (vhd->header.loc[i].code != PLAT_CODE_NONE)
			hlocs++;
	}

	if (hlocs) {
		err  = vhd_journal_read_locators(j, &locators, &locs);
		if (err)
			return err;

		if (hlocs != locs) {
			err = -EINVAL;
			goto out;
		}
	}

	err  = vhd_journal_read_bat(j, &vhd->bat);
	if (err)
		goto out;

	if (vhd_has_batmap(vhd)) {
		err  = vhd_journal_read_batmap(j, &vhd->batmap);
		if (err)
			goto out;
	}

restore:
	err  = vhd_journal_restore_footer(j, &vhd->footer);
	if (err)
		goto out;

	if (!vhd_type_dynamic(vhd))
		goto out;

	err  = vhd_journal_restore_header(j, &vhd->header);
	if (err)
		goto out;

	if (locs) {
		err = vhd_journal_restore_locators(j, locators, locs);
		if (err)
			goto out;
	}

	err  = vhd_journal_restore_bat(j, &vhd->bat);
	if (err)
		goto out;

	if (vhd_has_batmap(vhd)) {
		err  = vhd_journal_restore_batmap(j, &vhd->batmap);
		if (err)
			goto out;
	}

	err = 0;

out:
	if (locators) {
		for (i = 0; i < locs; i++)
			free(locators[i]);
		free(locators);
	}

	if (!err)
		ftruncate(vhd->fd,
			  j->header.footer_offset + sizeof(vhd_footer_t));

	return err;
}

static int
vhd_journal_disable_vhd(vhd_journal_t *j)
{
	int err;
	vhd_context_t *vhd;

	vhd = &j->vhd;

	err = vhd_get_footer(vhd);
	if (err)
		return err;

	memcpy(&vhd->footer.cookie,
	       VHD_POISON_COOKIE, sizeof(vhd->footer.cookie));

	err = vhd_write_footer(vhd, &vhd->footer);
	if (err)
		return err;

	return 0;
}

static int
vhd_journal_enable_vhd(vhd_journal_t *j)
{
	int err;
	vhd_context_t *vhd;

	vhd = &j->vhd;

	err = vhd_get_footer(vhd);
	if (err)
		return err;

	memcpy(&vhd->footer.cookie, HD_COOKIE, sizeof(vhd->footer.cookie));

	err = vhd_write_footer(vhd, &vhd->footer);
	if (err)
		return err;

	return 0;
}

int
vhd_journal_close(vhd_journal_t *j)
{
	if (j->jfd)
		close(j->jfd);

	vhd_close(&j->vhd);
	free(j->jname);

	return 0;
}

int
vhd_journal_remove(vhd_journal_t *j)
{
	int err;

	err = vhd_journal_enable_vhd(j);
	if (err)
		return err;

	if (j->jfd) {
		close(j->jfd);
		unlink(j->jname);
	}

	vhd_close(&j->vhd);
	free(j->jname);

	return 0;
}

int
vhd_journal_open(vhd_journal_t *j, const char *file)
{
	int err;
	vhd_context_t *vhd;

	memset(j, 0, sizeof(vhd_journal_t));

	j->jfd = -1;
	vhd    = &j->vhd;

	if (asprintf(&j->jname, "%s.journal", file) == -1) {
		j->jname = NULL;
		return -ENOMEM;
	}

	j->jfd = open(j->jname, O_LARGEFILE | O_RDWR);
	if (j->jfd == -1) {
		err = -errno;
		goto fail;
	}

	vhd->fd = open(file, O_LARGEFILE | O_RDWR | O_DIRECT);
	if (vhd->fd == -1) {
		err = -errno;
		goto fail;
	}

	err = vhd_journal_read_journal_header(j, &j->header);
	if (err)
		goto fail;

	err = vhd_journal_restore_metadata(j);
	if (err)
		goto fail;

	close(vhd->fd);
	free(vhd->bat.bat);
	free(vhd->batmap.map);

	err = vhd_open(vhd, file, O_LARGEFILE | O_RDWR | O_DIRECT);
	if (err)
		goto fail;

	err = vhd_get_bat(vhd);
	if (err)
		goto fail;

	if (vhd_has_batmap(vhd)) {
		err = vhd_get_batmap(vhd);
		if (err)
			goto fail;
	}

	err = vhd_journal_disable_vhd(j);
	if (err)
		goto fail;

	return 0;

fail:
	vhd_journal_close(j);
	return err;
}

int
vhd_journal_create(vhd_journal_t *j, const char *file)
{
	char *buf;
	int i, err;
	size_t size;
	off64_t off;

	memset(j, 0, sizeof(vhd_journal_t));
	j->jfd = -1;

	if (asprintf(&j->jname, "%s.journal", file) == -1) {
		err = -ENOMEM;
		goto fail1;
	}

	if (access(j->jname, F_OK) == 0) {
		err = -EEXIST;
		goto fail1;
	}

	j->jfd = open(j->jname,
		      O_CREAT | O_TRUNC | O_LARGEFILE | O_RDWR, 0644);
	if (j->jfd == -1) {
		err = -errno;
		goto fail1;
	}

	err = vhd_open(&j->vhd, file, O_LARGEFILE | O_RDWR | O_DIRECT);
	if (err)
		goto fail1;

	err = vhd_get_bat(&j->vhd);
	if (err)
		goto fail2;

	if (vhd_has_batmap(&j->vhd)) {
		err = vhd_get_batmap(&j->vhd);
		if (err)
			goto fail2;
	}

	err = vhd_journal_add_journal_header(j);
	if (err)
		goto fail2;

	err = vhd_journal_add_metadata(j);
	if (err)
		goto fail2;

	err = vhd_journal_disable_vhd(j);
	if (err)
		goto fail2;

	return 0;

fail1:
	if (j->jfd != -1) {
		close(j->jfd);
		unlink(j->jname);
	}
	free(j->jname);
	memset(j, 0, sizeof(vhd_journal_t));

	return err;

fail2:
	vhd_journal_remove(j);
	return err;
}

int
vhd_journal_add_block(vhd_journal_t *j, uint32_t block, char mode)
{
	int err;
	char *buf;
	off64_t off;
	size_t size;
	uint32_t blk;
	vhd_context_t *vhd;

	buf = NULL;
	vhd = &j->vhd;

	if (!vhd_type_dynamic(vhd))
		return -EINVAL;

	err = vhd_get_bat(vhd);
	if (err)
		return err;

	if (block >= vhd->bat.entries)
		return -ERANGE;

	blk = vhd->bat.bat[block];
	if (blk == DD_BLK_UNUSED)
		return 0;

	off = blk << VHD_SECTOR_SHIFT;

	if (mode & VHD_JOURNAL_METADATA) {
		size = vhd->bm_secs << VHD_SECTOR_SHIFT;

		err  = vhd_read_bitmap(vhd, block, &buf);
		if (err)
			return err;

		err  = vhd_journal_update(j, off, buf, size,
					  VHD_JOURNAL_ENTRY_TYPE_DATA);

		free(buf);

		if (err)
			return err;
	}

	if (mode & VHD_JOURNAL_DATA) {
		off += (vhd->bm_secs << VHD_SECTOR_SHIFT);
		size = vhd->spb << VHD_SECTOR_SHIFT;

		err  = vhd_read_block(vhd, block, &buf);
		if (err)
			return err;

		err  = vhd_journal_update(j, off, buf, size,
					  VHD_JOURNAL_ENTRY_TYPE_DATA);
		free(buf);

		if (err)
			return err;
	}

	return 0;
}

/*
 * commit indicates the transaction completed 
 * successfully and we can remove the undo log
 */
int
vhd_journal_commit(vhd_journal_t *j)
{
	int err;

	j->header.entries = 0;
	err = vhd_journal_write_header(j, &j->header);
	if (err)
		return err;

	err = vhd_journal_truncate(j, sizeof(vhd_journal_header_t));
	if (err)
		return -errno;

	return 0;
}

/*
 * revert indicates the transaction failed
 * and we should revert any changes via the undo log
 */
int
vhd_journal_revert(vhd_journal_t *j)
{
	char *buf;
	int i, err;
	vhd_context_t *vhd;
	vhd_journal_entry_t entry;

	err = 0;
	vhd = &j->vhd;
	buf = NULL;

	err = vhd_journal_seek(j, sizeof(vhd_journal_header_t), SEEK_SET);
	if (err)
		return err;

	for (i = 0; i < j->header.entries; i++) {
		err = vhd_journal_read_entry(j, &entry);
		if (err)
			goto end;

		err = posix_memalign((void **)&buf,
				     VHD_SECTOR_SIZE, entry.size);
		if (err) {
			err = -err;
			buf = NULL;
			goto end;
		}

		err = vhd_journal_read(j, buf, entry.size);
		if (err)
			goto end;

		err = vhd_journal_validate_entry_data(&entry, buf);
		if (err)
			goto end;

		err = vhd_seek(vhd, entry.offset, SEEK_SET);
		if (err)
			goto end;

		err = vhd_write(vhd, buf, entry.size);
		if (err)
			goto end;

		err = 0;

	end:
		free(buf);
		buf = NULL;
		if (err)
			break;
	}

	if (err)
		return err;

	err = ftruncate(vhd->fd,
			j->header.footer_offset + sizeof(vhd_footer_t));
	if (err)
		err = -errno;

	return err;
}