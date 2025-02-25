/* netscreen.c
 *
 * Juniper NetScreen snoop output parser
 * Created by re-using a lot of code from cosine.c
 * Copyright (c) 2007 by Sake Blok <sake@euronet.nl>
 *
 * Wiretap Library
 * Copyright (c) 1998 by Gilbert Ramirez <gram@alumni.rice.edu>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"
#include "wtap-int.h"
#include "netscreen.h"
#include "file_wrappers.h"

#include <stdlib.h>
#include <string.h>

/* XXX TODO:
 *
 * o  Construct a list of interfaces, with interface names, give
 *    them link-layer types based on the interface name and packet
 *    data, and supply interface IDs with each packet (i.e., make
 *    this supply a pcapng-style set of interfaces and associate
 *    packets with interfaces).  This is probably the right way
 *    to "Pass the interface names and the traffic direction to either
 *    the frame-structure, a pseudo-header or use PPI."  See the
 *    message at
 *
 *        https://www.wireshark.org/lists/wireshark-dev/200708/msg00029.html
 *
 *    to see whether any further discussion is still needed. I suspect
 *    it doesn't; pcapng existed at the time, as per the final
 *    message in that thread:
 *
 *        https://www.wireshark.org/lists/wireshark-dev/200708/msg00039.html
 *
 *    but I don't think we fully *supported* it at that point.  Now
 *    that we do, we have the infrastructure to support this, except
 *    that we currently have no way to translate interface IDs to
 *    interface names in the "frame" dissector or to supply interface
 *    information as part of the packet metadata from Wiretap modules.
 *    That should be fixed so that we can show interface information,
 *    such as the interface name, in packet dissections from, for example,
 *    pcapng captures.
 */

static gboolean info_line(const gchar *line);
static gint64 netscreen_seek_next_packet(wtap *wth, int *err, gchar **err_info,
	char *hdr);
static gboolean netscreen_check_file_type(wtap *wth, int *err,
	gchar **err_info);
static gboolean netscreen_read(wtap *wth, wtap_rec *rec, Buffer *buf,
	int *err, gchar **err_info, gint64 *data_offset);
static gboolean netscreen_seek_read(wtap *wth, gint64 seek_off,
	wtap_rec *rec, Buffer *buf, int *err, gchar **err_info);
static gboolean parse_netscreen_packet(FILE_T fh, wtap_rec *rec,
	Buffer* buf, char *line, int *err, gchar **err_info);
static int parse_single_hex_dump_line(char* rec, guint8 *buf,
	guint byte_offset);

static int netscreen_file_type_subtype = -1;

void register_netscreen(void);

/* Returns TRUE if the line appears to be a line with protocol info.
   Otherwise it returns FALSE. */
static gboolean info_line(const gchar *line)
{
	int i=NETSCREEN_SPACES_ON_INFO_LINE;

	while (i-- > 0) {
		if (g_ascii_isspace(*line)) {
			line++;
			continue;
		} else {
			return FALSE;
		}
	}
	return TRUE;
}

/* Seeks to the beginning of the next packet, and returns the
   byte offset. Copy the header line to hdr. Returns -1 on failure,
   and sets "*err" to the error and sets "*err_info" to null or an
   additional error string. */
static gint64 netscreen_seek_next_packet(wtap *wth, int *err, gchar **err_info,
    char *hdr)
{
	gint64 cur_off;
	char buf[NETSCREEN_LINE_LENGTH];

	while (1) {
		cur_off = file_tell(wth->fh);
		if (cur_off == -1) {
			/* Error */
			*err = file_error(wth->fh, err_info);
			return -1;
		}
		if (file_gets(buf, sizeof(buf), wth->fh) == NULL) {
			/* EOF or error. */
			*err = file_error(wth->fh, err_info);
			break;
		}
		if (strstr(buf, NETSCREEN_REC_MAGIC_STR1) ||
		    strstr(buf, NETSCREEN_REC_MAGIC_STR2)) {
			(void) g_strlcpy(hdr, buf, NETSCREEN_LINE_LENGTH);
			return cur_off;
		}
	}
	return -1;
}

/* Look through the first part of a file to see if this is
 * NetScreen snoop output.
 *
 * Returns TRUE if it is, FALSE if it isn't or if we get an I/O error;
 * if we get an I/O error, "*err" will be set to a non-zero value and
 * "*err_info" is set to null or an additional error string.
 */
static gboolean netscreen_check_file_type(wtap *wth, int *err, gchar **err_info)
{
	char	buf[NETSCREEN_LINE_LENGTH];
	guint	reclen, line;

	buf[NETSCREEN_LINE_LENGTH-1] = '\0';

	for (line = 0; line < NETSCREEN_HEADER_LINES_TO_CHECK; line++) {
		if (file_gets(buf, NETSCREEN_LINE_LENGTH, wth->fh) == NULL) {
			/* EOF or error. */
			*err = file_error(wth->fh, err_info);
			return FALSE;
		}

		reclen = (guint) strlen(buf);
		if (reclen < MIN(strlen(NETSCREEN_HDR_MAGIC_STR1), strlen(NETSCREEN_HDR_MAGIC_STR2))) {
			continue;
		}

		if (strstr(buf, NETSCREEN_HDR_MAGIC_STR1) ||
		    strstr(buf, NETSCREEN_HDR_MAGIC_STR2)) {
			return TRUE;
		}
	}
	*err = 0;
	return FALSE;
}


wtap_open_return_val netscreen_open(wtap *wth, int *err, gchar **err_info)
{

	/* Look for a NetScreen snoop header line */
	if (!netscreen_check_file_type(wth, err, err_info)) {
		if (*err != 0 && *err != WTAP_ERR_SHORT_READ)
			return WTAP_OPEN_ERROR;
		return WTAP_OPEN_NOT_MINE;
	}

	if (file_seek(wth->fh, 0L, SEEK_SET, err) == -1)	/* rewind */
		return WTAP_OPEN_ERROR;

	wth->file_encap = WTAP_ENCAP_UNKNOWN;
	wth->file_type_subtype = netscreen_file_type_subtype;
	wth->snapshot_length = 0; /* not known */
	wth->subtype_read = netscreen_read;
	wth->subtype_seek_read = netscreen_seek_read;
	wth->file_tsprec = WTAP_TSPREC_100_MSEC;

	return WTAP_OPEN_MINE;
}

/* Find the next packet and parse it; called from wtap_read(). */
static gboolean netscreen_read(wtap *wth, wtap_rec *rec, Buffer *buf,
    int *err, gchar **err_info, gint64 *data_offset)
{
	gint64		offset;
	char		line[NETSCREEN_LINE_LENGTH];

	/* Find the next packet */
	offset = netscreen_seek_next_packet(wth, err, err_info, line);
	if (offset < 0)
		return FALSE;

	/* Parse the header and convert the ASCII hex dump to binary data */
	if (!parse_netscreen_packet(wth->fh, rec, buf, line, err, err_info))
		return FALSE;

	/*
	 * If the per-file encapsulation isn't known, set it to this
	 * packet's encapsulation.
	 *
	 * If it *is* known, and it isn't this packet's encapsulation,
	 * set it to WTAP_ENCAP_PER_PACKET, as this file doesn't
	 * have a single encapsulation for all packets in the file.
	 */
	if (wth->file_encap == WTAP_ENCAP_UNKNOWN)
		wth->file_encap = rec->rec_header.packet_header.pkt_encap;
	else {
		if (wth->file_encap != rec->rec_header.packet_header.pkt_encap)
			wth->file_encap = WTAP_ENCAP_PER_PACKET;
	}

	*data_offset = offset;
	return TRUE;
}

/* Used to read packets in random-access fashion */
static gboolean
netscreen_seek_read(wtap *wth, gint64 seek_off,	wtap_rec *rec, Buffer *buf,
	int *err, gchar **err_info)
{
	char		line[NETSCREEN_LINE_LENGTH];

	if (file_seek(wth->random_fh, seek_off, SEEK_SET, err) == -1) {
		return FALSE;
	}

	if (file_gets(line, NETSCREEN_LINE_LENGTH, wth->random_fh) == NULL) {
		*err = file_error(wth->random_fh, err_info);
		if (*err == 0) {
			*err = WTAP_ERR_SHORT_READ;
		}
		return FALSE;
	}

	return parse_netscreen_packet(wth->random_fh, rec, buf, line,
	    err, err_info);
}

/* Parses a packet record header. There are a few possible formats:
 *
 * XXX list extra formats here!
6843828.0: trust(o) len=98:00121ebbd132->00600868d659/0800
              192.168.1.1 -> 192.168.1.10/6
              vhl=45, tos=00, id=37739, frag=0000, ttl=64 tlen=84
              tcp:ports 2222->2333, seq=3452113890, ack=1540618280, flag=5018/ACK
              00 60 08 68 d6 59 00 12 1e bb d1 32 08 00 45 00     .`.h.Y.....2..E.
              00 54 93 6b 00 00 40 06 63 dd c0 a8 01 01 c0 a8     .T.k..@.c.......
              01 0a 08 ae 09 1d cd c3 13 e2 5b d3 f8 28 50 18     ..........[..(P.
              1f d4 79 21 00 00 e7 76 89 64 16 e2 19 0a 80 09     ..y!...v.d......
              31 e7 04 28 04 58 f3 d9 b1 9f 3d 65 1a db d8 61     1..(.X....=e...a
              2c 21 b6 d3 20 60 0c 8c 35 98 88 cf 20 91 0e a9     ,!...`..5.......
              1d 0b                                               ..


 */
static gboolean
parse_netscreen_packet(FILE_T fh, wtap_rec *rec, Buffer* buf,
    char *line, int *err, gchar **err_info)
{
	int		pkt_len;
	int		sec;
	int		dsec;
	char		cap_int[NETSCREEN_MAX_INT_NAME_LENGTH];
	char		direction[2];
	char		cap_src[13];
	char		cap_dst[13];
	guint8		*pd;
	gchar		*p;
	int		n, i = 0;
	int		offset = 0;
	gchar		dststr[13];

	rec->rec_type = REC_TYPE_PACKET;
	rec->block = wtap_block_create(WTAP_BLOCK_PACKET);
	rec->presence_flags = WTAP_HAS_TS|WTAP_HAS_CAP_LEN;
	/* Suppress compiler warnings */
	memset(cap_int, 0, sizeof(cap_int));
	memset(cap_dst, 0, sizeof(cap_dst));

	if (sscanf(line, "%9d.%9d: %15[a-z0-9/:.-](%1[io]) len=%9d:%12s->%12s/",
		   &sec, &dsec, cap_int, direction, &pkt_len, cap_src, cap_dst) < 5) {
		*err = WTAP_ERR_BAD_FILE;
		*err_info = g_strdup("netscreen: Can't parse packet-header");
		return -1;
	}
	if (pkt_len < 0) {
		*err = WTAP_ERR_BAD_FILE;
		*err_info = g_strdup("netscreen: packet header has a negative packet length");
		return FALSE;
	}
	if ((guint)pkt_len > WTAP_MAX_PACKET_SIZE_STANDARD) {
		/*
		 * Probably a corrupt capture file; don't blow up trying
		 * to allocate space for an immensely-large packet.
		 */
		*err = WTAP_ERR_BAD_FILE;
		*err_info = ws_strdup_printf("netscreen: File has %u-byte packet, bigger than maximum of %u",
		    (guint)pkt_len, WTAP_MAX_PACKET_SIZE_STANDARD);
		return FALSE;
	}

	/*
	 * If direction[0] is 'o', the direction is NETSCREEN_EGRESS,
	 * otherwise it's NETSCREEN_INGRESS.
	 */

	rec->ts.secs  = sec;
	rec->ts.nsecs = dsec * 100000000;
	rec->rec_header.packet_header.len = pkt_len;

	/* Make sure we have enough room for the packet */
	ws_buffer_assure_space(buf, pkt_len);
	pd = ws_buffer_start_ptr(buf);

	while(1) {

		/* The last packet is not delimited by an empty line, but by EOF
		 * So accept EOF as a valid delimiter too
		 */
		if (file_gets(line, NETSCREEN_LINE_LENGTH, fh) == NULL) {
			break;
		}

		/*
		 * Skip blanks.
		 * The number of blanks is not fixed - for wireless
		 * interfaces, there may be 14 extra spaces before
		 * the hex data.
		 */
		for (p = &line[0]; g_ascii_isspace(*p); p++)
			;
		/* packets are delimited with empty lines */
		if (*p == '\0') {
			break;
		}

		n = parse_single_hex_dump_line(p, pd, offset);

		/* the smallest packet has a length of 6 bytes, if
		 * the first hex-data is less then check whether
		 * it is a info-line and act accordingly
		 */
		if (offset == 0 && n < 6) {
			if (info_line(line)) {
				if (++i <= NETSCREEN_MAX_INFOLINES) {
					continue;
				}
			} else {
				*err = WTAP_ERR_BAD_FILE;
				*err_info = g_strdup("netscreen: cannot parse hex-data");
				return FALSE;
			}
		}

		/* If there is no more data and the line was not empty,
		 * then there must be an error in the file
		 */
		if (n == -1) {
			*err = WTAP_ERR_BAD_FILE;
			*err_info = g_strdup("netscreen: cannot parse hex-data");
			return FALSE;
		}

		/* Adjust the offset to the data that was just added to the buffer */
		offset += n;

		/* If there was more hex-data than was announced in the len=x
		 * header, then there must be an error in the file
		 */
		if (offset > pkt_len) {
			*err = WTAP_ERR_BAD_FILE;
			*err_info = g_strdup("netscreen: too much hex-data");
			return FALSE;
		}
	}

	/*
	 * Determine the encapsulation type, based on the
	 * first 4 characters of the interface name
	 *
	 * XXX	convert this to a 'case' structure when adding more
	 *	(non-ethernet) interfacetypes
	 */
	if (strncmp(cap_int, "adsl", 4) == 0) {
		/* The ADSL interface can be bridged with or without
		 * PPP encapsulation. Check whether the first six bytes
		 * of the hex data are the same as the destination mac
		 * address in the header. If they are, assume ethernet
		 * LinkLayer or else PPP
		 */
		snprintf(dststr, 13, "%02x%02x%02x%02x%02x%02x",
		   pd[0], pd[1], pd[2], pd[3], pd[4], pd[5]);
		if (strncmp(dststr, cap_dst, 12) == 0)
			rec->rec_header.packet_header.pkt_encap = WTAP_ENCAP_ETHERNET;
		else
			rec->rec_header.packet_header.pkt_encap = WTAP_ENCAP_PPP;
		}
	else if (strncmp(cap_int, "seri", 4) == 0)
		rec->rec_header.packet_header.pkt_encap = WTAP_ENCAP_PPP;
	else
		rec->rec_header.packet_header.pkt_encap = WTAP_ENCAP_ETHERNET;

	rec->rec_header.packet_header.caplen = offset;

	return TRUE;
}

/* Take a string representing one line from a hex dump, with leading white
 * space removed, and converts the text to binary data. We place the bytes
 * in the buffer at the specified offset.
 *
 * Returns number of bytes successfully read, -1 if bad.  */
static int
parse_single_hex_dump_line(char* rec, guint8 *buf, guint byte_offset)
{
	int num_items_scanned;
	guint8 character;
	guint8 byte;


	for (num_items_scanned = 0; num_items_scanned < 16; num_items_scanned++) {
		character = *rec++;
		if (character >= '0' && character <= '9')
			byte = character - '0' + 0;
		else if (character >= 'A' && character <= 'F')
			byte = character - 'A' + 0xA;
		else if (character >= 'a' && character <= 'f')
			byte = character - 'a' + 0xa;
		else if (character == ' ' || character == '\r' || character == '\n' || character == '\0') {
			/* Nothing more to parse */
			break;
		} else
			return -1; /* not a hex digit, space before ASCII dump, or EOL */
		byte <<= 4;
		character = *rec++ & 0xFF;
		if (character >= '0' && character <= '9')
			byte += character - '0' + 0;
		else if (character >= 'A' && character <= 'F')
			byte += character - 'A' + 0xA;
		else if (character >= 'a' && character <= 'f')
			byte += character - 'a' + 0xa;
		else
			return -1; /* not a hex digit */
		buf[byte_offset + num_items_scanned] = byte;
		character = *rec++ & 0xFF;
		if (character == '\0' || character == '\r' || character == '\n') {
			/* Nothing more to parse */
			break;
		} else if (character != ' ') {
			/* not space before ASCII dump */
			return -1;
		}
	}
	if (num_items_scanned == 0)
		return -1;

	return num_items_scanned;
}

static const struct supported_block_type netscreen_blocks_supported[] = {
	/*
	 * We support packet blocks, with no comments or other options.
	 */
	{ WTAP_BLOCK_PACKET, MULTIPLE_BLOCKS_SUPPORTED, NO_OPTIONS_SUPPORTED }
};

static const struct file_type_subtype_info netscreen_info = {
	"NetScreen snoop text file", "netscreen", "txt", NULL,
	FALSE, BLOCKS_SUPPORTED(netscreen_blocks_supported),
	NULL, NULL, NULL
};

void register_netscreen(void)
{
	netscreen_file_type_subtype = wtap_register_file_type_subtype(&netscreen_info);

	/*
	 * Register name for backwards compatibility with the
	 * wtap_filetypes table in Lua.
	 */
	wtap_register_backwards_compatibility_lua_name("NETSCREEN",
	    netscreen_file_type_subtype);
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 8
 * tab-width: 8
 * indent-tabs-mode: t
 * End:
 *
 * vi: set shiftwidth=8 tabstop=8 noexpandtab:
 * :indentSize=8:tabSize=8:noTabs=false:
 */
