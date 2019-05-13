#ifndef ODYSSEY_IO_H
#define ODYSSEY_IO_H

/*
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
*/

#include "macro.h"

typedef struct od_io od_io_t;

struct od_io
{
	od_readahead_t  readahead;
	machine_cond_t *on_read;
	machine_cond_t *on_write;
	machine_io_t   *io;
};

static inline void
od_io_init(od_io_t *io)
{
	io->io       = NULL;
	io->on_read  = NULL;
	io->on_write = NULL;
	od_readahead_init(&io->readahead);
}

static inline void
od_io_free(od_io_t *io)
{
	od_readahead_free(&io->readahead);
    if (io->on_read)
	    machine_cond_free(io->on_read);
    if (io->on_write)
	    machine_cond_free(io->on_write);
}

static inline char*
od_io_error(od_io_t *io)
{
	return machine_error(io->io);
}

static inline int
od_io_prepare(od_io_t *io, machine_io_t *io_obj, int readahead)
{
	io->io = io_obj;
	int rc;
	rc = od_readahead_prepare(&io->readahead, readahead);
	if (rc == -1)
		return -1;
	io->on_read = machine_cond_create();
	if (io->on_read == NULL)
		return -1;
	io->on_write = machine_cond_create();
	if (io->on_write == NULL)
		return -1;
	return 0;
}

static inline int
od_io_close(od_io_t *io)
{
	if (io->io == NULL)
		return -1;
	int rc = machine_close(io->io);
	machine_io_free(io->io);
	io->io = NULL;
	return rc;
}

static inline int
od_io_attach(od_io_t *io)
{
	return machine_io_attach(io->io);
}

static inline int
od_io_detach(od_io_t *io)
{
	return machine_io_detach(io->io);
}

static inline int
od_io_read(od_io_t *io, char *dest, int size, uint32_t time_ms)
{
	int read_started = 0;
	int pos = 0;
	int rc;
	for (;;)
	{
		int unread;
		unread = od_readahead_unread(&io->readahead);
		if (unread > 0) {
			int to_read = unread;
			if (to_read > size)
				to_read = size;
			memcpy(dest + pos, od_readahead_pos_read(&io->readahead), to_read);
			size -= to_read;
			pos  += to_read;
			od_readahead_pos_read_advance(&io->readahead, to_read);
		} else {
			od_readahead_reuse(&io->readahead);
		}

		if (size == 0)
			break;

		if (! read_started)
			machine_cond_signal(io->on_read);

		for (;;)
		{
			rc = machine_cond_wait(io->on_read, time_ms);
			if (rc == -1)
				return -1;

			int left;
			left = od_readahead_left(&io->readahead);

			rc = machine_read_raw(io->io, od_readahead_pos(&io->readahead), left);
			if (rc <= 0) {
				/* retry using read condition wait */
				int errno_ = machine_errno();
				if (errno_ == EAGAIN || errno_ == EWOULDBLOCK || errno_ == EINTR) {
					if (! read_started) {
						rc = machine_read_start(io->io, io->on_read);
						if (rc == -1)
							return -1;
						read_started = 1;
					}
					continue;
				}
				/* error or unexpected eof */
				return -1;
			}

			od_readahead_pos_advance(&io->readahead, rc);
			break;
		}
	}

	if (read_started) {
		rc = machine_read_stop(io->io);
		if (rc == -1)
			return -1;
	}

	return 0;
}

static inline machine_msg_t*
od_read_startup(od_io_t *io, uint32_t time_ms)
{
	uint32_t header;
	int rc;
	rc = od_io_read(io, (char*)&header, sizeof(header), time_ms);
	if (rc == -1)
		return NULL;

	uint32_t size;
	size = kiwi_read_startup_size((char*)&header, sizeof(header));

	machine_msg_t *msg;
	msg = machine_msg_create(sizeof(header) + size);
	if (msg == NULL)
		return NULL;

	char *dest;
	dest = machine_msg_data(msg);
	memcpy(dest, &header, sizeof(header));
	dest += sizeof(header);

	rc = od_io_read(io, dest, size, time_ms);
	if (rc == -1) {
		machine_msg_free(msg);
		return NULL;
	}

	return msg;
}

/*
 * This macro lists the backend message types that could be "long" (more
 * than a couple of kilobytes).
 */
#define VALID_LONG_MESSAGE_TYPE(id) \
	((id) == 'T' || (id) == 'D' || (id) == 'd' || (id) == 'V' || /* BE messages */\
	(id) == 'E' || (id) == 'N' || (id) == 'A'|| /* BE messages */\
	(id) == 'B' || (id) == 'P' || (id) == 'Q') /* FE messages */

static inline machine_msg_t*
od_read(od_io_t *io, uint32_t time_ms)
{
	kiwi_header_t header;
	int rc;
	rc = od_io_read(io, (char*)&header, sizeof(header), time_ms);
	if (rc == -1)
		return NULL;

	uint32_t size;
	size = kiwi_read_size((char*)&header, sizeof(header));

	if ( od_unlikely(
			size < sizeof(uint32_t) ||
		    header.type < 0x20 ||
		    (size > 30000 && !VALID_LONG_MESSAGE_TYPE(header.type)))
		    ) {
		/*
		 * This is not a postgres protocol v3 message
		 * We should drop connection ASAP
		 * Validation is performed per PostgreSQL impl
		 * For reference see
		 * https://github.com/postgres/postgres/blob/7bac3acab4d5c3f2c35aa3a7bea08411d83fd5bc/src/interfaces/libpq/fe-protocol3.c#L91-L100
		 */
		return NULL;
	}
	size -= sizeof(uint32_t);

	machine_msg_t *msg;
	msg = machine_msg_create(sizeof(header) + size);
	if (msg == NULL)
		return NULL;

	char *dest;
	dest = machine_msg_data(msg);
	memcpy(dest, &header, sizeof(header));
	dest += sizeof(header);

	rc = od_io_read(io, dest, size, time_ms);
	if (rc == -1) {
		machine_msg_free(msg);
		return NULL;
	}

	return msg;
}

static inline int
od_write(od_io_t *io, machine_msg_t *msg)
{
	return machine_write(io->io, msg, UINT32_MAX);
}

#endif /* ODYSSEY_IO_H */
