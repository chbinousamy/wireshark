/* packet-http3.c
 * Routines for HTTP/3 dissection
 * Copyright 2019, Peter Wu <peter@lekensteyn.nl>
 * Copyright 2023, Omer Shapira <oesh@github.com>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * https://tools.ietf.org/html/draft-ietf-quic-http-29
 * https://tools.ietf.org/html/draft-ietf-quic-qpack-16
 *
 * Depends on the QUIC dissector for providing a reassembled stream of data, see
 * packet-quic.c for details about supported QUIC draft versions.
 * Depends on nghttp3 for HTTP header dissection.
 * Currently supported HTTP/3 versions: h3-23 up to and including h3-29.
 */

#include <config.h>

#include <epan/expert.h>
#include <epan/packet.h>
#include <epan/proto_data.h>
#include <epan/to_str.h>
#include <stdint.h>
#include <string.h>
#include <wsutil/pint.h>

#include <epan/conversation_table.h>
#include <epan/dissectors/packet-http.h> /* for getting status reason-phrase */

#include "packet-quic.h"
#include "packet-tls-utils.h"
#include "wsutil/wmem/wmem_user_cb.h"

#include <epan/decode_as.h>
#include <epan/reassemble.h>
#include <epan/uat.h>

#ifdef HAVE_NGHTTP3
#include <nghttp3/nghttp3.h>
#endif

/* Comment out to disable dissector debug */
// #define HTTP3_DISSECTOR_DEBUG

#if defined(HTTP3_DISSECTOR_DEBUG)
#define HTTP3_DISSECTOR_DPRINTF(fmt, ...)                                                                              \
    printf("HTTP3 DEBUG: %s:%d (%s) " fmt "\n", __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#define HTTP3_DISSECTOR_DPRINTF(fmt, ...)                                                                              \
    do {                                                                                                               \
    } while (0)
#endif

void proto_reg_handoff_http3(void);
void proto_register_http3(void);

static dissector_handle_t http3_handle;

static int proto_http3                = -1;
static int hf_http3_stream_uni        = -1;
static int hf_http3_stream_uni_type   = -1;
static int hf_http3_stream_bidi       = -1;
static int hf_http3_push_id           = -1;
static int hf_http3_frame             = -1;
static int hf_http3_frame_type        = -1;
static int hf_http3_frame_length      = -1;
static int hf_http3_frame_payload     = -1;

static int hf_http3_data = -1;

//static int hf_http3_headers                 = -1;
static int hf_http3_headers_count           = -1;
static int hf_http3_header                  = -1;
static int hf_http3_header_length           = -1;
static int hf_http3_header_name_length      = -1;
static int hf_http3_header_name             = -1;
static int hf_http3_header_value_length     = -1;
static int hf_http3_header_value            = -1;
static int hf_http3_header_request_full_uri = -1;

static int hf_http3_header_qpack_blocked               = -1;
static int hf_http3_header_qpack_blocked_stream_rcint  = -1;
static int hf_http3_header_qpack_blocked_decoder_wicnt = -1;
//static int hf_http3_header_qpack_fatal                 = -1;

#ifdef HAVE_NGHTTP3
/* Static HTTP3 headers */
static int hf_http3_headers_status                      = -1;
static int hf_http3_headers_path                        = -1;
static int hf_http3_headers_method                      = -1;
static int hf_http3_headers_scheme                      = -1;
static int hf_http3_headers_accept                      = -1;
static int hf_http3_headers_accept_charset              = -1;
static int hf_http3_headers_accept_encoding             = -1;
static int hf_http3_headers_accept_language             = -1;
static int hf_http3_headers_accept_ranges               = -1;
static int hf_http3_headers_access_control_allow_origin = -1;
static int hf_http3_headers_age                         = -1;
static int hf_http3_headers_allow                       = -1;
static int hf_http3_headers_authorization               = -1;
static int hf_http3_headers_authority                   = -1;
static int hf_http3_headers_cache_control               = -1;
static int hf_http3_headers_content_disposition         = -1;
static int hf_http3_headers_content_encoding            = -1;
static int hf_http3_headers_content_language            = -1;
static int hf_http3_headers_content_length              = -1;
static int hf_http3_headers_content_location            = -1;
static int hf_http3_headers_content_range               = -1;
static int hf_http3_headers_content_type                = -1;
static int hf_http3_headers_cookie                      = -1;
static int hf_http3_headers_date                        = -1;
static int hf_http3_headers_etag                        = -1;
static int hf_http3_headers_expect                      = -1;
static int hf_http3_headers_expires                     = -1;
static int hf_http3_headers_from                        = -1;
static int hf_http3_headers_if_match                    = -1;
static int hf_http3_headers_if_modified_since           = -1;
static int hf_http3_headers_if_none_match               = -1;
static int hf_http3_headers_if_range                    = -1;
static int hf_http3_headers_if_unmodified_since         = -1;
static int hf_http3_headers_last_modified               = -1;
static int hf_http3_headers_link                        = -1;
static int hf_http3_headers_location                    = -1;
static int hf_http3_headers_max_forwards                = -1;
static int hf_http3_headers_proxy_authenticate          = -1;
static int hf_http3_headers_proxy_authorization         = -1;
static int hf_http3_headers_range                       = -1;
static int hf_http3_headers_referer                     = -1;
static int hf_http3_headers_refresh                     = -1;
static int hf_http3_headers_retry_after                 = -1;
static int hf_http3_headers_server                      = -1;
static int hf_http3_headers_set_cookie                  = -1;
static int hf_http3_headers_strict_transport_security   = -1;
static int hf_http3_headers_user_agent                  = -1;
static int hf_http3_headers_vary                        = -1;
static int hf_http3_headers_via                         = -1;
static int hf_http3_headers_www_authenticate            = -1;
#endif

//static int hf_http3_qpack                                    = -1;
static int hf_http3_qpack_encoder                            = -1;
//static int hf_http3_qpack_encoder_length                     = -1;
static int hf_http3_qpack_encoder_icnt                       = -1;
static int hf_http3_qpack_encoder_icnt_inc                   = -1;
//static int hf_http3_qpack_encoder_opcode                     = -1;
static int hf_http3_qpack_encoder_opcode_insert_indexed      = -1;
static int hf_http3_qpack_encoder_opcode_insert_indexed_ref  = -1;
static int hf_http3_qpack_encoder_opcode_insert_indexed_val  = -1;
static int hf_http3_qpack_encoder_opcode_insert_indexed_hval = -1;
static int hf_http3_qpack_encoder_opcode_insert              = -1;
static int hf_http3_qpack_encoder_opcode_insert_name         = -1;
static int hf_http3_qpack_encoder_opcode_insert_hname        = -1;
static int hf_http3_qpack_encoder_opcode_insert_val          = -1;
static int hf_http3_qpack_encoder_opcode_insert_hval         = -1;
static int hf_http3_qpack_encoder_opcode_duplicate           = -1;
//static int hf_http3_qpack_encoder_opcode_duplicate_val       = -1;
static int hf_http3_qpack_encoder_opcode_dtable_cap          = -1;
static int hf_http3_qpack_encoder_opcode_dtable_cap_val      = -1;

static int hf_http3_settings                          = -1;
static int hf_http3_settings_identifier               = -1;
static int hf_http3_settings_value                    = -1;
static int hf_http3_settings_qpack_max_table_capacity = -1;
static int hf_http3_settings_max_field_section_size   = -1;
static int hf_http3_settings_qpack_blocked_streams    = -1;
static int hf_http3_settings_extended_connect         = -1;
static int hf_http3_settings_webtransport             = -1;
static int hf_http3_settings_h3_datagram              = -1;
static int hf_http3_settings_h3_datagram_draft04      = -1;
static int hf_http3_priority_update_element_id        = -1;
static int hf_http3_priority_update_field_value       = -1;

/* QPACK dissection EIs */
//static expert_field ei_http3_qpack_enc_update = EI_INIT;
static expert_field ei_http3_qpack_failed     = EI_INIT;
/* HTTP3 dissection EIs */
static expert_field ei_http3_unknown_stream_type = EI_INIT;
//static expert_field ei_http3_data_not_decoded    = EI_INIT;
/* Encoded data EIs */
static expert_field ei_http3_header_encoded_state = EI_INIT;
/* HTTP3 header decoding EIs */
static expert_field ei_http3_header_decoding_failed    = EI_INIT;
static expert_field ei_http3_header_decoding_blocked   = EI_INIT;
static expert_field ei_http3_header_decoding_no_output = EI_INIT;

/* Initialize the subtree pointers */
static gint ett_http3                       = -1;
static gint ett_http3_stream_uni            = -1;
static gint ett_http3_stream_bidi           = -1;
static gint ett_http3_frame                 = -1;
static gint ett_http3_settings              = -1;
static gint ett_http3_headers               = -1;
static gint ett_http3_headers_qpack_blocked = -1;
static gint ett_http3_qpack_update          = -1;
static gint ett_http3_qpack_opcode          = -1;

/**
 * HTTP3 header constants.
 * The below constants are used for dissecting the
 * code. This is not an exahustive list.
 */
#define HTTP3_HEADER_NAME_CONTENT_ENCODING  "content-encoding"
#define HTTP3_HEADER_NAME_CONTENT_TYPE      "content-type"
#define HTTP3_HEADER_NAME_TRANSFER_ENCODING "transfer-encoding"
#define HTTP3_HEADER_NAME_AUTHORITY         ":authority"
#define HTTP3_HEADER_NAME_METHOD            ":method"
#define HTTP3_HEADER_NAME_PATH              ":path"
#define HTTP3_HEADER_NAME_SCHEME            ":scheme"
#define HTTP3_HEADER_NAME_STATUS            ":status"

#define HTTP3_HEADER_METHOD_CONNECT         "CONNECT"
#define HTTP3_HEADER_STATUS_PARTIAL_CONTENT "206"

#define HTTP3_HEADER_UNKNOWN                "<unknown>"

/**
 * Unidirectional stream types.
 * https://tools.ietf.org/html/draft-ietf-quic-http-29#section-6.2
 * https://tools.ietf.org/html/draft-ietf-quic-qpack-16#section-4.2
 */
enum http3_stream_type {
    HTTP3_STREAM_TYPE_CONTROL,
    HTTP3_STREAM_TYPE_PUSH,
    HTTP3_STREAM_TYPE_QPACK_ENCODER,
    HTTP3_STREAM_TYPE_QPACK_DECODER,
};

/**
 * Unidirectional stream types (62-bit code space).
 * https://tools.ietf.org/html/draft-ietf-quic-http-29#section-11.2.4
 */
// clang-format off
static const val64_string http3_stream_types[] = {
    /* 0x00 - 0x3f Assigned via Standards Action or IESG Approval. */
    { 0x00, "Control Stream" },
    { 0x01, "Push Stream" },
    { 0x02, "QPACK Encoder Stream" },
    { 0x03, "QPACK Decoder Stream" },
    /* 0x40 - 0x3FFFFFFFFFFFFFFF Assigned via Specification Required policy */
    { 0, NULL }
};
// clang-format on

/**
 * Frame type codes (62-bit code space).
 * https://tools.ietf.org/html/draft-ietf-quic-http-29#section-11.2.1
 */
#define HTTP3_DATA                              0x0
#define HTTP3_HEADERS                           0x1
#define HTTP3_CANCEL_PUSH                       0x3
#define HTTP3_SETTINGS                          0x4
#define HTTP3_PUSH_PROMISE                      0x5
#define HTTP3_GOAWAY                            0x7
#define HTTP3_MAX_PUSH_ID                       0xD
#define HTTP3_WEBTRANSPORT_BISTREAM             0x41
#define HTTP3_WEBTRANSPORT_UNISTREAM            0x54
#define HTTP3_PRIORITY_UPDATE_REQUEST_STREAM    0xF0700
#define HTTP3_PRIORITY_UPDATE_PUSH_STREAM       0xF0701

static const val64_string http3_frame_types[] = {
    /* 0x00 - 0x3f Assigned via Standards Action or IESG Approval. */
    { HTTP3_DATA, "DATA" },
    { HTTP3_HEADERS, "HEADERS" },
    { 0x02, "Reserved" },       // "PRIORITY" in draft-22 and before
    { HTTP3_CANCEL_PUSH, "CANCEL_PUSH" },
    { HTTP3_SETTINGS, "SETTINGS" },
    { HTTP3_PUSH_PROMISE, "PUSH_PROMISE" },
    { 0x06, "Reserved" },
    { HTTP3_GOAWAY, "GOAWAY" },
    { 0x08, "Reserved" },
    { 0x09, "Reserved" },
    { HTTP3_MAX_PUSH_ID, "MAX_PUSH_ID" },
    { 0x0e, "Reserved" }, // "DUPLICATE_PUSH" in draft-26 and before
    { HTTP3_WEBTRANSPORT_BISTREAM, "WEBTRANSPORT_BISTREAM" }, // draft-ietf-webtrans-http3-03
    { HTTP3_WEBTRANSPORT_UNISTREAM, "WEBTRANSPORT_UNISTREAM" }, // draft-ietf-webtrans-http3-03
    { HTTP3_PRIORITY_UPDATE_REQUEST_STREAM, "PRIORITY_UPDATE" }, // RFC 9218
    { HTTP3_PRIORITY_UPDATE_PUSH_STREAM, "PRIORITY_UPDATE" }, // RFC 9218
    /* 0x40 - 0x3FFFFFFFFFFFFFFF Assigned via Specification Required policy */
    { 0, NULL }
};

/**
 * Settings parameter type codes (62-bit code space).
 * https://tools.ietf.org/html/draft-ietf-quic-http-29#name-http-2-settings-parameters
 */
#define HTTP3_QPACK_MAX_TABLE_CAPACITY          0x01
#define HTTP3_SETTINGS_MAX_FIELD_SECTION_SIZE   0x06
#define HTTP3_QPACK_BLOCKED_STREAMS             0x07
#define HTTP3_EXTENDED_CONNECT                  0x08        /* https://datatracker.ietf.org/doc/draft-ietf-httpbis-h3-websockets */
#define HTTP3_H3_DATAGRAM                       0x33             // rfc9297
#define HTTP3_H3_DATAGRAM_DRAFT04               0xffd277 // draft-ietf-masque-h3-datagram-04
#define HTTP3_WEBTRANSPORT                      0x2b603742      // draft-ietf-webtrans-http3-03

static const val64_string http3_settings_vals[] = {
    { HTTP3_QPACK_MAX_TABLE_CAPACITY, "Max Table Capacity" },
    { HTTP3_SETTINGS_MAX_FIELD_SECTION_SIZE, "Max Field Section Size" },
    { HTTP3_QPACK_BLOCKED_STREAMS, "Blocked Streams" },
    { HTTP3_EXTENDED_CONNECT, "Extended CONNECT" },
    { HTTP3_WEBTRANSPORT, "Enable WebTransport" },
    { HTTP3_H3_DATAGRAM, "Enable Datagram" },
    { HTTP3_H3_DATAGRAM_DRAFT04, "Enable Datagram Draft04" },
    { 0, NULL }
};

/**
 * QPACK encoder stream opcodes.
 */
#define QPACK_OPCODE_MASK                       0xE0
#define QPACK_OPCODE_INSERT_INDEXED             0x80
#define QPACK_OPCODE_INSERT                     0x40
#define QPACK_OPCODE_SET_DTABLE_CAP             0x20
#define QPACK_OPCODE_DUPLICATE                  0x00

#define QPACK_HUFFMAN_5_STRING                  0x20
#define QPACK_HUFFMAN_6_STRING                  0x40
#define QPACK_HUFFMAN_7_STRING                  q0x80

typedef enum _http3_stream_dir {
    FROM_CLIENT_TO_SERVER = 0,
    FROM_SERVER_TO_CLIENT = 1,
} http3_stream_dir;

/**
 * Essential data structures.
 */

/**
 * HTTP3 stream info - contains information about HTTP3 stream.
 * HTTP3 streams roughly correspond to QUIC streams, with the
 * HTTP3 Server Push being an exception to the rule.
 */
typedef struct _http3_stream_info {
    guint64           id;                 /**< HTTP3 stream id */
    quic_stream_info *quic_stream_info;   /**< Underlying QUIC stream info */
    guint64           uni_stream_type;    /**< Unidirectional stream type */
    guint64           broken_from_offset; /**< Unrecognized stream starting at offset (if non-zero). */
    http3_stream_dir  direction;
} http3_stream_info_t;

/**
 * HTTP3 session info - contains information about the HTTP3 session.
 * HTTP3 sessions roughly correspond to QUIC connections, at least
 * until the dissector will support connection migration and/or
 * Multipath QUIC. When that happens, a single HTTP3 session would
 * be mapped to multiple QUIC connections, or to multiple QUIC
 * paths (in the MP-QUIC terminology).
 */

typedef void *qpack_decoder_t;
typedef void *qpack_decoder_ctx_t;
typedef struct _http3_session_info {
    guint           id;
    qpack_decoder_t qpack_decoder[2]; /**< Decoders for outgoing/incoming QPACK streams. */
} http3_session_info_t;

/**
 * Lookup or create new HTTP3 session object for the pinfo.
 */
static http3_session_info_t *http3_session_lookup_or_create(packet_info *pinfo);

/**
 * HTTP3 Header dissection support.
 */
#define QPACK_MAX_DTABLE_SIZE   65536   /**< Max size of the QPACK dynamic table. */
#define QPACK_MAX_BLOCKED       512     /**< Upper limit on number of streams blocked on QPACK updates. */

/**
 *  Decompressed header field definition.
 *  Header field definitions are cached separately,
 *  to preserve memory.
 */
typedef struct _http3_header_field_def {
    const guint8 *name;
    guint         name_len;
} http3_header_field_def_t;

/**
 * HTTP3 header field.
 *
 * The header field contains two sections:
 * - encoded points to the location of the encoded field in the *original* packet TVB.
 * - decoded points to the formatted header string, which is allocated in a cache map,
 *   to conserve memory.
 * The decoded fields are used to create an auxillary TVB which will
 * be used for dissection of decoded header values.
 */
typedef struct _http3_header_field {
    struct {
        guint len;
        guint offset;
    } encoded;
    struct {
        const guint8 *pstr;
        guint         pstr_len;
    } decoded;
    http3_header_field_def_t *def;
} http3_header_field_t;

/**
 * HTTP3 encoded header data block.
 *
 * This helper structure is used to support header dissection.
 */
typedef struct _header_block_encoded_iter {
    guint8 *bytes;
    guint32 len;
    guint32 pos;
} header_block_encoded_iter_t;

#define HEADER_BLOCK_ENC_ITER_PTR(hdata)                                                                               \
    ((hdata)->encoded.bytes == NULL                                                                                    \
         ? NULL                                                                                                        \
         : ((hdata)->encoded.pos == (hdata)->encoded.len) ? NULL : (hdata)->encoded.bytes + (hdata)->encoded.pos)

#define HEADER_BLOCK_ENC_ITER_REMAINING(hdata)                                                                         \
    ((hdata)->encoded.bytes == NULL ? 0 : ((hdata)->encoded.len - (hdata)->encoded.pos))

#define HEADER_BLOCK_ENC_ITER_INC(hdata, nread)                                                                        \
    do {                                                                                                               \
        if ((hdata)) {                                                                                                 \
            (hdata)->encoded.pos += (nread);                                                                           \
            DISSECTOR_ASSERT((hdata)->encoded.pos <= (hdata)->encoded.len);                                            \
        }                                                                                                              \
    } while (0)

/**
 * HTTP3 header data block.
 *
 * The data block corresponds to contents of a single HTTP3 HEADERS frame.
 * If a packet contains multiple HTTP3 HEADERS frames,
 * the corresponding blocks will be chained using the `next'
 * pointer. In this case, individual headers blocks
 * will be identified by the `offset' field.
 */
typedef struct _http3_header_data {
    guint                       len;           /**< Length of the encoded headers block. */
    guint                       offset;        /**< Offset of the headers block in the pinfo TVB. */
    wmem_array_t *              header_fields; /**< List of header fields contained in the header block. */
    header_block_encoded_iter_t encoded;       /**< Used for dissection, not allocated. */
    struct _http3_header_data * next;          /**< Next pointer in the chain. */
} http3_header_data_t;

/**
 * File-scoped context.
 * This data structure is used to maintain file-scoped
 * lookup tables. It is reset when the file-scoped
 * allocator is exited.
 */
typedef struct _http3_file_local_ctx {
    wmem_map_t *conn_info_map;
#ifdef HAVE_NGHTTP3
    wmem_map_t *hdr_cache_map;
    wmem_map_t *hdr_def_cache_map;
#endif
} http3_file_local_ctx;

/**
 * @function http3_get_file_local_ctx
 * @abstract  Will create a new instance for the first time
 *            the file is visited.
 *            This function is not intended to be invked directly,
 *            but should be used via the `HTTP3_CONN_INFO_MAP` et. al. below.
 * @returns file-local context.
 */
static http3_file_local_ctx *http3_get_file_local_ctx(void);

#define HTTP3_CONN_INFO_MAP http3_get_file_local_ctx()->conn_info_map

#ifdef HAVE_NGHTTP3
#define HTTP3_HEADER_CACHE http3_get_file_local_ctx()->hdr_cache_map
#define HTTP3_HEADER_NAME_CACHE http3_get_file_local_ctx()->hdr_def_cache_map
#endif

/**
 * Check whether the argument represents a reserved code point,
 * for Stream Type, Frame Type, Error Code, etc.
 */
static inline gboolean
http3_is_reserved_code(guint64 stream_type)
{
    return (stream_type - 0x21) % 0x1f == 0;
}

/**
 * Attempt to parse QUIC-encoded variable integer.
 */
static gboolean
try_get_quic_varint(tvbuff_t *tvb, int offset, guint64 *value, int *lenvar)
{
    if (tvb_reported_length_remaining(tvb, offset) == 0) {
        return FALSE;
    }
    gint len = 1 << (tvb_get_guint8(tvb, offset) >> 6);
    if (tvb_reported_length_remaining(tvb, offset) < len) {
        return FALSE;
    }
    *lenvar = len;
    if (value) {
        gint n = (gint)tvb_get_varint(tvb, offset, -1, value, ENC_VARINT_QUIC);
        DISSECTOR_ASSERT_CMPINT(n, ==, len);
    }
    return TRUE;
}

/**
 * Return the size of entire HTTP/3 frame.
 */
static int
get_http3_frame_size(tvbuff_t *tvb, int offset)
{
    int     type_size, length_size;
    guint64 frame_length;

    if (!try_get_quic_varint(tvb, offset, NULL, &type_size)) {
        return 0;
    }
    offset += type_size;

    if (!try_get_quic_varint(tvb, offset, &frame_length, &length_size)) {
        return 0;
    }

    guint64 frame_size = type_size + length_size + frame_length;
    if (frame_size > G_MAXINT32) {
        // We do not support such large frames.
        return 0;
    }
    return (int)frame_size;
}

/**
 * Check whether the pinfo contains at least one whole HTTP3 frame,
 * and adjust the pinfo desegmentation settings for the lower
 * layer (QUIC, generally) to continue the desegmentation process.
 */
static gboolean
http3_check_frame_size(tvbuff_t *tvb, packet_info *pinfo, int offset)
{
    int frame_size = get_http3_frame_size(tvb, offset);
    int remaining  = tvb_reported_length_remaining(tvb, offset);
    if (frame_size && frame_size <= remaining) {
        return TRUE;
    }

    pinfo->desegment_offset = offset;
    pinfo->desegment_len    = frame_size ? (frame_size - remaining) : DESEGMENT_ONE_MORE_SEGMENT;
    return FALSE;
}

/**
 * Functions to support decompression of HTTP3 headers.
 */
#ifdef HAVE_NGHTTP3
/**
 * File-scoped callback to release resources allocated for the QPACK
 * decoder.
 */
static bool
qpack_decoder_del_cb(wmem_allocator_t *allocator _U_, wmem_cb_event_t event _U_, void *user_data)
{
    nghttp3_qpack_decoder_del((nghttp3_qpack_decoder *)user_data);
    return FALSE;
}

/**
 * Memory allocation callbacks for nghttp3_qpack functionality.
 */
static void *
http3_nghttp3_malloc(size_t size, void *user_data _U_)
{
    return wmem_alloc0(wmem_file_scope(), size);
}

static void
http3_nghttp3_free(void *ptr, void *user_data _U_)
{
    wmem_free(wmem_file_scope(), ptr);
}

static void *
http3_nghttp3_calloc(size_t nmemb, size_t size, void *user_data _U_)
{
    return wmem_alloc0(wmem_file_scope(), nmemb * size);
}

static void *
http3_nghttp3_realloc(void *ptr, size_t size, void *user_data _U_)
{
    return wmem_realloc(wmem_file_scope(), ptr, size);
}

static nghttp3_mem g_qpack_mem_allocator = {
    .user_data = NULL,
    .malloc    = http3_nghttp3_malloc,
    .free      = http3_nghttp3_free,
    .calloc    = http3_nghttp3_calloc,
    .realloc   = http3_nghttp3_realloc,
};

static nghttp3_mem *
qpack_mem_allocator(wmem_allocator_t *allocator _U_, int debug _U_)
{
    nghttp3_mem *mem;
    mem = &g_qpack_mem_allocator;
    return mem;
}

/**
 * Initialization routine for the http3_session object.
 * Invoked during the creation of the new http3_session.
 */
static void
http3_initialize_qpack_decoders(http3_session_info_t *http3_session)
{
    for (int dir = 0; dir < 2; dir++) {
        nghttp3_qpack_decoder **pdecoder = (nghttp3_qpack_decoder **)&(http3_session->qpack_decoder[dir]);
        nghttp3_qpack_decoder_new(pdecoder, QPACK_MAX_DTABLE_SIZE, QPACK_MAX_BLOCKED,
                                  qpack_mem_allocator(wmem_file_scope(), 1));
        nghttp3_qpack_decoder_set_max_dtable_capacity(*pdecoder, QPACK_MAX_DTABLE_SIZE);
        wmem_register_callback(wmem_file_scope(), qpack_decoder_del_cb, *pdecoder);
    }
}

static GHashTable *header_fields_hash = NULL;

static const char *
cid_to_string(const quic_cid_t *cid)
{
    if (cid->len == 0) {
        return "(none)";
    }
    char *str = (char *)wmem_alloc0(wmem_packet_scope(), 2 * cid->len + 1);
    bytes_to_hexstr(str, cid->cid, cid->len);
    return str;
}

static http3_header_data_t *
http3_get_header_data(packet_info *pinfo, guint offset)
{
    http3_header_data_t *data, *prev = NULL;

    data = (http3_header_data_t *)p_get_proto_data(wmem_file_scope(), pinfo, proto_http3, 0);

    /*
     * Attempt to find existing header data block.
     * In most cases, data will be `NULL'
     * and this loop won't be visited.
     */
    while (data != NULL) {
        if (data->offset == offset) {
            /*
             * We found the matching data. Return it.
             */
            return data;
        }
        prev = data;
        data = data->next;
    }

    /*
     * We did not find header data matching the offset.
     * Allocate a new header data block, and initialize
     * the offset marker.
     */
    data         = wmem_new0(wmem_file_scope(), http3_header_data_t);
    data->offset = offset;

    /*
     * Check whether the newly allocated data should be linked
     * to the tail of existing header block chain, or whether
     * it is the head of a new header block chain.
     */
    if (prev != NULL) {
        prev->next = data;
    } else {
        p_add_proto_data(wmem_file_scope(), pinfo, proto_http3, 0, data);
    }

    return data;
}

static inline http3_stream_dir
http3_packet_get_direction(http3_stream_info_t *http3_stream)
{
    return http3_stream->quic_stream_info->from_server
        ? FROM_CLIENT_TO_SERVER
        : FROM_SERVER_TO_CLIENT;
}

char *http3_header_pstr = NULL;

static void
try_append_method_path_info(packet_info *pinfo, proto_tree *tree, const gchar *method_header_value,
                            const gchar *path_header_value, const gchar *authority_header_value)
{
    if (method_header_value != NULL) {
        if ((strcmp(method_header_value, "CONNECT_UDP") == 0) || (strcmp(method_header_value, "CONNECT") == 0)) {
            if (authority_header_value != NULL) {
                col_append_sep_fstr(pinfo->cinfo, COL_INFO, ": ", "%s %s", method_header_value, authority_header_value);
            }
        } else {
            if (path_header_value != NULL) {
                /* append request information to info column (for example, HEADERS: GET /demo/1.jpg) */
                col_append_sep_fstr(pinfo->cinfo, COL_INFO, ": ", "%s %s", method_header_value, path_header_value);
                /* append request information to Stream node */
                proto_item_append_text(tree, ", %s %s", method_header_value, path_header_value);
            }
        }
    }
}

static proto_item *
try_add_named_header_field(proto_tree *tree, tvbuff_t *tvb, int offset, guint32 length, const char *header_name,
                           const char *header_value)
{
    int                hf_id = -1;
    header_field_info *hfi;
    proto_item        *ti = NULL;

    const gint *entry = (const gint *)g_hash_table_lookup(header_fields_hash, header_name);
    if (entry == NULL) {
        return NULL;
    }

    hf_id = *entry;

    hfi = proto_registrar_get_nth(hf_id);
    DISSECTOR_ASSERT(hfi != NULL);

    if (FT_IS_UINT32(hfi->type)) {
        guint32 value;
        if (ws_strtou32(header_value, NULL, &value)) {
            ti = proto_tree_add_uint(tree, hf_id, tvb, offset, length, value);
        }
    } else if (FT_IS_UINT(hfi->type)) {
        guint64 value;
        if (ws_strtou64(header_value, NULL, &value)) {
            ti = proto_tree_add_uint64(tree, hf_id, tvb, offset, length, value);
        }
    } else {
        ti = proto_tree_add_item(tree, hf_id, tvb, offset, length, ENC_BIG_ENDIAN);
    }
    return ti;
}

static int
dissect_http3_headers(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, guint tvb_offset, guint offset,
                      http3_stream_info_t *http3_stream)
{
    const gchar  *authority_header_value = NULL;
    const gchar  *method_header_value    = NULL;
    const gchar  *path_header_value      = NULL;
    const gchar  *scheme_header_value    = NULL;
    const guint8 *header_name;
    const guint8 *header_value;

    gint                          length = 0;
    guint32                       header_name_length;
    guint32                       header_value_length;
    http3_header_data_t           *header_data;
    http3_session_info_t          *http3_session;
    http3_stream_dir              packet_direction;
    int                           header_len = 0, hoffset = 0;
    nghttp3_qpack_decoder         *decoder;
    nghttp3_qpack_stream_context  *sctx = NULL;
    proto_item                    *header, *ti, *ti_named_field;
    proto_tree                    *header_tree, *blocked_rcint_tree;
    tvbuff_t                      *header_tvb;

    http3_session = http3_session_lookup_or_create(pinfo);
    header_data   = http3_get_header_data(pinfo, offset);

    HTTP3_DISSECTOR_DPRINTF("pdinfo visited=%d", PINFO_FD_VISITED(pinfo));

    if (!PINFO_FD_VISITED(pinfo)) {
        /*
         * This packet has not been processed yet, which means this is
         *  the first linear scan.  We do header decompression only
         *  once in linear scan and cache the result.  If we don't
         *  cache, already processed data will be fed into decompressor
         *  again and again since dissector will be called randomly.
         *  This makes context out-of-sync.
         */

        nghttp3_qpack_stream_context_new(&sctx, http3_stream->id, nghttp3_mem_default());

        length           = tvb_reported_length_remaining(tvb, tvb_offset);
        packet_direction = http3_packet_get_direction(http3_stream);
        decoder          = http3_session->qpack_decoder[packet_direction];

        DISSECTOR_ASSERT(decoder);
        DISSECTOR_ASSERT(header_data);
        DISSECTOR_ASSERT(header_data->encoded.bytes == NULL);
        DISSECTOR_ASSERT(header_data->encoded.len == 0);
        DISSECTOR_ASSERT(header_data->header_fields == NULL);

        header_data->encoded.bytes = tvb_memdup(wmem_file_scope(), tvb, tvb_offset, length);
        header_data->encoded.pos   = 0;
        header_data->encoded.len   = length;

        HTTP3_DISSECTOR_DPRINTF("Header data: %p %d %d\n", header_data->encoded.bytes, header_data->encoded.pos,
                                header_data->encoded.len);

        proto_tree_add_expert_format(tree, pinfo, &ei_http3_header_encoded_state, tvb, tvb_offset, 0,
                                     "HTTP3 encoded headers - bytes %p pos %d len %d", header_data->encoded.bytes,
                                     header_data->encoded.pos, header_data->encoded.len);
        /*
         * Attempt to decode headers.
         *
         * TODO: This may incorrectly put headers that were blocked
         * for packet k in the past to this packet n. We will deal with this later
         */
        while (HEADER_BLOCK_ENC_ITER_REMAINING(header_data)) {
            nghttp3_qpack_nv nv;
            guint8           flags;

            HTTP3_DISSECTOR_DPRINTF("%p %p:%d decode decoder=%p sctx=%p", header_data->encoded.bytes,
                                    HEADER_BLOCK_ENC_ITER_PTR(header_data),
                                    HEADER_BLOCK_ENC_ITER_REMAINING(header_data), decoder, sctx);

            gint32 nread = (gint32)nghttp3_qpack_decoder_read_request(decoder, sctx, &nv, &flags,
                                                                      HEADER_BLOCK_ENC_ITER_PTR(header_data),
                                                                      HEADER_BLOCK_ENC_ITER_REMAINING(header_data), 1);

            if (nread < 0) {
                /*
                 * This should be signaled up.
                 */
                HTTP3_DISSECTOR_DPRINTF("Early return nread=%d err=%s", nread, nghttp3_strerror(nread));
                proto_tree_add_expert_format(tree, pinfo, &ei_http3_header_decoding_failed, tvb, tvb_offset, 0,
                                             "QPACK error decoder %p ctx %p flags %" PRIu8 " error %d (%s)", decoder,
                                             sctx, flags, nread, nghttp3_strerror((int)nread));
                break;
            }

            /*
             * Check whether the QPACK decoder is blocked on QPACK encoder stream.
             */
            if (flags & NGHTTP3_QPACK_DECODE_FLAG_BLOCKED) {
                guint64 ricnt, wicnt;

                ricnt = nghttp3_qpack_stream_context_get_ricnt(sctx);
                wicnt = nghttp3_qpack_decoder_get_icnt(decoder);
                ti    = proto_tree_add_boolean(tree, hf_http3_header_qpack_blocked, tvb, tvb_offset, 0, true);
                proto_item_set_generated(ti);
                blocked_rcint_tree = proto_item_add_subtree(ti, ett_http3_headers_qpack_blocked);
                ti = proto_tree_add_uint(blocked_rcint_tree, hf_http3_header_qpack_blocked_stream_rcint, tvb,
                                         tvb_offset, 0, (guint32)ricnt);
                proto_item_set_generated(ti);
                proto_tree_add_uint(blocked_rcint_tree, hf_http3_header_qpack_blocked_decoder_wicnt, tvb, tvb_offset, 0,
                                    (guint32)wicnt);
                proto_tree_add_expert_format(tree, pinfo, &ei_http3_header_decoding_blocked, tvb, tvb_offset, 0,
                                             "QPACK - blocked decoder %p ctx %p flags=%" PRIu8 " ricnt=%" PRIu64
                                             " wicnt=%" PRIu64 " error %d (%s)",
                                             decoder, sctx, flags, ricnt, wicnt, nread, nghttp3_strerror((int)nread));
                HTTP3_DISSECTOR_DPRINTF("Early return nread=%d blocked=%" PRIu8 " ricnt=%" PRIu64 " wicnt=%" PRIu64 "",
                                        nread, flags, ricnt, wicnt);
                break;
            }

            /*
             * Check whether the decoder has emitted header data.
             */
            if (flags & NGHTTP3_QPACK_DECODE_FLAG_EMIT) {
                http3_header_field_t        *out;
                http3_header_field_def_t    *def;
                char                        *cached_pstr;
                nghttp3_vec                 name_vec;
                nghttp3_vec                 value_vec;
                guint32                     name_len;
                guint8                      *name;
                guint32                     value_len;
                guint8                      *value;
                guint32                     pstr_len;

                HTTP3_DISSECTOR_DPRINTF("Emit nread=%d flags=%" PRIu8 "", nread, flags);

                if (header_data->header_fields == NULL) {
                    header_data->header_fields = wmem_array_new(wmem_file_scope(), sizeof(http3_header_field_t));
                }

                name_vec  = nghttp3_rcbuf_get_buf(nv.name);
                name_len  = (guint32)name_vec.len;
                name      = name_vec.base;
                value_vec = nghttp3_rcbuf_get_buf(nv.value);
                value_len = (guint32)value_vec.len;
                value     = value_vec.base;

                HTTP3_DISSECTOR_DPRINTF("HTTP header: %.*s: %.*s", name_len, name, value_len, value);

                pstr_len          = (name_len + value_len + 4 + 4);
                http3_header_pstr = (char *)wmem_realloc(wmem_file_scope(), http3_header_pstr, pstr_len);
                phton32(&http3_header_pstr[0], name_len);
                memcpy(&http3_header_pstr[4], name, name_len);
                phton32(&http3_header_pstr[4 + name_len], value_len);
                memcpy(&http3_header_pstr[4 + name_len + 4], value, value_len);

                /* Lookup a field definition, or create one if needed */
                def = (http3_header_field_def_t *)wmem_map_lookup(HTTP3_HEADER_NAME_CACHE, http3_header_pstr);
                if (def == NULL) {
                    char *def_name = NULL;
                    def_name       = (char *)wmem_realloc(wmem_file_scope(), def_name, name_len + 1);
                    memcpy(def_name, name, name_len);

                    def           = wmem_new0(wmem_file_scope(), http3_header_field_def_t);
                    def->name_len = name_len;
                    def->name     = (const char *)def_name;

                    wmem_map_insert(HTTP3_HEADER_NAME_CACHE, http3_header_pstr, def);
                }

                /* Create an output field and add it to the headers array */
                out      = wmem_new0(wmem_file_scope(), http3_header_field_t);
                out->def = def;

                cached_pstr = (char *)wmem_map_lookup(HTTP3_HEADER_CACHE, http3_header_pstr);
                if (cached_pstr) {
                    out->decoded.pstr = cached_pstr;
                } else {
                    out->decoded.pstr = http3_header_pstr;
                    wmem_map_insert(HTTP3_HEADER_CACHE, http3_header_pstr, http3_header_pstr);
                    http3_header_pstr = NULL;
                }
                out->decoded.pstr_len = pstr_len;

                wmem_array_append(header_data->header_fields, out, 1);

                /*
                 * Decrease the reference counts on the NGHTTP3 nv structure to avoid
                 * memory leaks.
                 */
                nghttp3_rcbuf_decref(nv.name);
                nghttp3_rcbuf_decref(nv.value);
            } else {
                proto_tree_add_expert_format(tree, pinfo, &ei_http3_header_decoding_no_output, tvb, tvb_offset, 0,
                                             "QPACK - nothing emitted decoder %p ctx %p flags %" PRIu8 " error %d (%s)",
                                             decoder, sctx, flags, nread, nghttp3_strerror((int)nread));
            }

            /*
             * Check whether the QPACK decoder has finished.
             */
            if (nread == 0 || (flags & NGHTTP3_QPACK_DECODE_FLAG_FINAL)) {
                break;
            }

            HEADER_BLOCK_ENC_ITER_INC(header_data, nread);
        }
    }

    if ((header_data->header_fields == NULL) || (wmem_array_get_count(header_data->header_fields) == 0)) {
        return tvb_offset;
    }

    header_tvb = tvb_new_composite();

    for (guint i = 0; i < wmem_array_get_count(header_data->header_fields); ++i) {
        http3_header_field_t    *in;
        tvbuff_t                *next_tvb;

        in = (http3_header_field_t *)wmem_array_index(header_data->header_fields, i);
        header_len += in->decoded.pstr_len;

        /* Now setup the tvb buffer to have the new data */
        next_tvb = tvb_new_child_real_data(tvb, in->decoded.pstr, in->decoded.pstr_len, in->decoded.pstr_len);
        tvb_composite_append(header_tvb, next_tvb);
    }

    tvb_composite_finalize(header_tvb);
    add_new_data_source(pinfo, header_tvb, "Decompressed Header");

    ti = proto_tree_add_uint(tree, hf_http3_header_length, header_tvb, hoffset, 1, header_len);
    proto_item_set_generated(ti);

    ti = proto_tree_add_uint(tree, hf_http3_headers_count, header_tvb, hoffset, 1,
                             wmem_array_get_count(header_data->header_fields));
    proto_item_set_generated(ti);

    for (guint i = 0; i < wmem_array_get_count(header_data->header_fields); ++i) {
        http3_header_field_t *in;

        in = (http3_header_field_t *)wmem_array_index(header_data->header_fields, i);

        /* Populate tree with header name/value details. */
        /* Add 'Header' subtree with description. */
        header = proto_tree_add_item(tree, hf_http3_header, tvb, tvb_offset, in->encoded.len, ENC_NA);

        header_tree = proto_item_add_subtree(header, ett_http3_headers);

        /* header value length */
        proto_tree_add_item_ret_uint(header_tree, hf_http3_header_name_length, header_tvb, hoffset, 4,
                                     ENC_BIG_ENDIAN, &header_name_length);
        hoffset += 4;

        /* Add header name. */
        proto_tree_add_item_ret_string(header_tree, hf_http3_header_name, header_tvb, hoffset, header_name_length,
                                       ENC_ASCII | ENC_NA, pinfo->pool, &header_name);
        hoffset += header_name_length;

        /* header value length */
        proto_tree_add_item_ret_uint(header_tree, hf_http3_header_value_length, header_tvb, hoffset, 4,
                                     ENC_BIG_ENDIAN, &header_value_length);
        hoffset += 4;

        /* Add header value. */
        proto_tree_add_item_ret_string(header_tree, hf_http3_header_value, header_tvb, hoffset, header_value_length,
                                       ENC_ASCII | ENC_NA, pinfo->pool, &header_value);

        ti_named_field = try_add_named_header_field(header_tree, header_tvb, hoffset, header_value_length, header_name,
                                                    header_value);

        hoffset += header_value_length;

        proto_item_append_text(header, ": %s: %s", header_name, header_value);

        /* Display :method, :path and :status in info column (just like http1.1 dissector does)*/
        if (strcmp(header_name, HTTP3_HEADER_NAME_METHOD) == 0) {
            method_header_value = header_value;
            try_append_method_path_info(pinfo, tree, method_header_value, path_header_value, authority_header_value);
        } else if (strcmp(header_name, HTTP3_HEADER_NAME_PATH) == 0) {
            path_header_value = header_value;
            try_append_method_path_info(pinfo, tree, method_header_value, path_header_value, authority_header_value);
            http_add_path_components_to_tree(header_tvb, pinfo, ti_named_field, hoffset - header_value_length,
                                             header_value_length);
        } else if (strcmp(header_name, HTTP3_HEADER_NAME_AUTHORITY) == 0) {
            authority_header_value = header_value;
            try_append_method_path_info(pinfo, tree, method_header_value, path_header_value, authority_header_value);
        } else if (strcmp(header_name, HTTP3_HEADER_NAME_STATUS) == 0) {
            const gchar *reason_phase =
                val_to_str_const((guint)strtoul(header_value, NULL, 10), vals_http_status_code, "Unknown");
            /* append response status and reason phrase to info column (for example, HEADERS: 200 OK) */
            col_append_sep_fstr(pinfo->cinfo, COL_INFO, ": ", "%s %s", header_value, reason_phase);
            /* append response status and reason phrase to header_tree and Stream node */
            proto_item_append_text(header_tree, " %s", reason_phase);
            proto_item_append_text(tree, ", %s %s", header_value, reason_phase);
        } else if (strcmp(header_name, HTTP3_HEADER_NAME_AUTHORITY) == 0) {
            authority_header_value = header_value;
        } else if (strcmp(header_name, HTTP3_HEADER_NAME_SCHEME) == 0) {
            scheme_header_value = header_value;
        }

        tvb_offset += in->encoded.len;
    }

    /*
     * Use the `:authority' Header as an indication that this packet is a request.
     */
    if (authority_header_value) {
        proto_item *e_ti;
        gchar      *uri;

        /*
         * https://www.ietf.org/rfc/rfc9114.html#name-request-pseudo-header-field
         *
         * All HTTP/3 requests MUST include exactly one value for the `:method',
         * `:scheme', and `:path' pseudo-header fields, unless the request is
         * a `CONNECT' request; see Section 4.4.
         */
        if (method_header_value && strcmp(method_header_value, HTTP3_HEADER_METHOD_CONNECT) == 0) {
            uri = wmem_strdup(wmem_packet_scope(), authority_header_value);
        } else {
            uri = wmem_strdup_printf(wmem_packet_scope(), "%s://%s%s", scheme_header_value, authority_header_value,
                                     path_header_value);
        }
        e_ti = proto_tree_add_string(tree, hf_http3_header_request_full_uri, tvb, 0, 0, uri);
        proto_item_set_url(e_ti);
        proto_item_set_generated(e_ti);
    }

    if (sctx) {
        nghttp3_qpack_stream_context_del(sctx);
    }

    return tvb_offset;
}
#endif /* HAVE_NGHTTP3 */

static http3_session_info_t *
http3_session_new(void)
{
    http3_session_info_t *http3_session;

    http3_session = wmem_new0(wmem_file_scope(), http3_session_info_t);

#ifdef HAVE_NGHTTP3
    http3_initialize_qpack_decoders(http3_session);
#endif

    return http3_session;
}

static http3_session_info_t *
http3_session_lookup_or_create(packet_info *pinfo)
{
    http3_session_info_t *http3_session;

    /* First, try to look up the session by initial QUIC DCID */
    quic_cid_t initial_dcid = {0};
    if (quic_conn_data_get_conn_client_dcid_initial(pinfo, &initial_dcid)) {
        /* Look up the session data in the conn map */
        http3_session = (http3_session_info_t *)wmem_map_lookup(HTTP3_CONN_INFO_MAP, &initial_dcid);
        if (http3_session == NULL) {
            quic_cid_t *dcid_p = wmem_memdup(wmem_file_scope(), &initial_dcid, sizeof(initial_dcid));
            http3_session = http3_session_new();
            wmem_map_insert(HTTP3_CONN_INFO_MAP, dcid_p, http3_session);
        }
    } else {
        /* Initial DCID can not be found, use the 5-tuple for lookup */
        conversation_t *conversation = find_or_create_conversation(pinfo);
        http3_session                = (http3_session_info_t *)conversation_get_proto_data(conversation, proto_http3);

        if (http3_session == NULL) {
            http3_session = http3_session_new();
            conversation_add_proto_data(conversation, proto_http3, http3_session);
        }
    }

    return http3_session;
}


static conversation_t *
http3_find_inner_conversation(packet_info *pinfo, http3_stream_info_t *http3_stream, void **ctx)
{
    conversation_t *inner_conv = NULL;

    if (http3_stream->quic_stream_info != NULL) {
        if (ctx) {
            *ctx = pinfo->conv_elements;
        }

        wmem_array_t *conversation_elements = wmem_array_new(pinfo->pool, sizeof(conversation_element_t));

        conversation_element_t h3_stream_addr = {
            .type     = CE_ADDRESS,
            .addr_val = (pinfo->srcport < pinfo->destport) ? pinfo->src : pinfo->dst,
        };
        wmem_array_append_one(conversation_elements, h3_stream_addr);

        conversation_element_t h3_stream_port = {
            .type     = CE_PORT,
            .port_val = (pinfo->srcport < pinfo->destport) ? pinfo->srcport : pinfo->destport,
        };
        wmem_array_append_one(conversation_elements, h3_stream_port);

        conversation_element_t h3_stream_quic_stream = {
            .type       = CE_UINT64,
            .uint64_val = http3_stream->id,
        };
        wmem_array_append_one(conversation_elements, h3_stream_quic_stream);

        conversation_element_t h3_stream_last = {
            .type                  = CE_CONVERSATION_TYPE,
            .conversation_type_val = CONVERSATION_LOG,
        };
        wmem_array_append_one(conversation_elements, h3_stream_last);

        pinfo->conv_elements = (conversation_element_t *)wmem_array_get_raw(conversation_elements);
        inner_conv           = find_conversation_pinfo(pinfo, 0);
        if (!inner_conv) {
            inner_conv = conversation_new_full(pinfo->fd->num, pinfo->conv_elements);
        }
    }

    return inner_conv;
}

static void
http3_reset_inner_conversation(packet_info *pinfo, void *ctx)
{
    if (ctx) {
        struct conversation_element *conv_elements = (struct conversation_element *)ctx;
        pinfo->conv_elements                       = conv_elements;
    }
}

static int
dissect_http3_data(tvbuff_t *tvb, packet_info *pinfo, proto_tree *http3_tree, guint offset _U_,
                   http3_stream_info_t *http3_stream)
{
    void                *saved_ctx = NULL;
    gint                remaining;
    conversation_t      *inner_conv _U_;
    proto_item          *ti_data _U_;

    remaining = tvb_reported_length(tvb);
    inner_conv = http3_find_inner_conversation(pinfo, http3_stream, &saved_ctx);
    ti_data    = proto_tree_add_item(http3_tree, hf_http3_data, tvb, offset, remaining, ENC_NA);
    http3_reset_inner_conversation(pinfo, saved_ctx);

    return tvb_reported_length(tvb);
}

/* Settings */
static int
dissect_http3_settings(tvbuff_t *tvb, packet_info *pinfo _U_, proto_tree *http3_tree, guint offset)
{
    guint64     settingsid, value;
    int         lenvar;
    proto_item  *ti_settings, *pi;
    proto_tree  *settings_tree;

    while (tvb_reported_length_remaining(tvb, offset) > 0) {
        ti_settings   = proto_tree_add_item(http3_tree, hf_http3_settings, tvb, offset, 2, ENC_NA);
        settings_tree = proto_item_add_subtree(ti_settings, ett_http3_settings);
        pi            = proto_tree_add_item_ret_varint(settings_tree, hf_http3_settings_identifier, tvb, offset, -1,
                                            ENC_VARINT_QUIC, &settingsid, &lenvar);
        /* Check if it is a GREASE Settings ID */
        if (http3_is_reserved_code(settingsid)) {
            proto_item_set_text(pi, "Type: GREASE (%#" PRIx64 ")", settingsid);
            proto_item_append_text(ti_settings, " - GREASE");
        } else {
            proto_item_append_text(ti_settings, " - %s",
                                   val64_to_str_const(settingsid, http3_settings_vals, "Unknown"));
        }

        offset += lenvar;
        proto_tree_add_item_ret_varint(settings_tree, hf_http3_settings_value, tvb, offset, -1, ENC_VARINT_QUIC, NULL,
                                       &lenvar);

        switch (settingsid) {
        case HTTP3_QPACK_MAX_TABLE_CAPACITY:
            proto_tree_add_item_ret_varint(settings_tree, hf_http3_settings_qpack_max_table_capacity, tvb, offset, -1,
                                           ENC_VARINT_QUIC, &value, &lenvar);
            proto_item_append_text(ti_settings, ": %" PRIu64, value);
            break;
        case HTTP3_SETTINGS_MAX_FIELD_SECTION_SIZE:
            proto_tree_add_item_ret_varint(settings_tree, hf_http3_settings_max_field_section_size, tvb, offset, -1,
                                           ENC_VARINT_QUIC, &value, &lenvar);
            proto_item_append_text(ti_settings, ": %" PRIu64, value);
            break;
        case HTTP3_QPACK_BLOCKED_STREAMS:
            proto_tree_add_item_ret_varint(settings_tree, hf_http3_settings_qpack_blocked_streams, tvb, offset, -1,
                                           ENC_VARINT_QUIC, &value, &lenvar);
            proto_item_append_text(ti_settings, ": %" PRIu64, value);
            break;
        case HTTP3_EXTENDED_CONNECT:
            proto_tree_add_item_ret_varint(settings_tree, hf_http3_settings_extended_connect, tvb, offset, -1,
                                           ENC_VARINT_QUIC, &value, &lenvar);
            proto_item_append_text(ti_settings, ": %" PRIu64, value);
            break;
        case HTTP3_WEBTRANSPORT:
            proto_tree_add_item_ret_varint(settings_tree, hf_http3_settings_webtransport, tvb, offset, -1,
                                           ENC_VARINT_QUIC, &value, &lenvar);
            proto_item_append_text(ti_settings, ": %" PRIu64, value);
            break;
        case HTTP3_H3_DATAGRAM:
            proto_tree_add_item_ret_varint(settings_tree, hf_http3_settings_h3_datagram, tvb, offset, -1,
                                           ENC_VARINT_QUIC, &value, &lenvar);
            proto_item_append_text(ti_settings, ": %" PRIu64, value);
            break;
        case HTTP3_H3_DATAGRAM_DRAFT04:
            proto_tree_add_item_ret_varint(settings_tree, hf_http3_settings_h3_datagram_draft04, tvb, offset, -1,
                                           ENC_VARINT_QUIC, &value, &lenvar);
            proto_item_append_text(ti_settings, ": %" PRIu64, value);
            break;
        default:
            /* No Default */
            break;
        }
        offset += lenvar;
    }

    return offset;
}

/**
 * Priority Update
 */
static int
dissect_http3_priority_update(tvbuff_t *tvb, packet_info *pinfo _U_, proto_tree *http3_tree, guint offset,
                              guint64 frame_length)
{
    guint64 priority_field_value_len;
    int     lenvar;

    proto_tree_add_item_ret_varint(http3_tree, hf_http3_priority_update_element_id, tvb, offset, -1, ENC_VARINT_QUIC,
                                   NULL, &lenvar);
    offset += lenvar;
    priority_field_value_len = frame_length - lenvar;

    proto_tree_add_item(http3_tree, hf_http3_priority_update_field_value, tvb, offset, (int)priority_field_value_len,
                        ENC_ASCII);
    offset += (int)priority_field_value_len;

    return offset;
}

static int
dissect_http3_frame(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, int offset, http3_stream_info_t *http3_stream)
{
    guint64     frame_type, frame_length;
    int         lenvar;
    proto_item  *ti_ft, *ti_ft_type;
    proto_tree  *ft_tree;
    const gchar *ft_display_name;

    ti_ft = proto_tree_add_item(tree, hf_http3_frame, tvb, offset, 1, ENC_NA);
    ft_tree = proto_item_add_subtree(ti_ft, ett_http3_frame);

    ti_ft_type = proto_tree_add_item_ret_varint(ft_tree, hf_http3_frame_type, tvb, offset, -1, ENC_VARINT_QUIC, &frame_type,
                                        &lenvar);
    offset += lenvar;
    if (http3_is_reserved_code(frame_type)) {
        proto_item_set_text(ti_ft_type, "Type: Reserved (%#" PRIx64 ")", frame_type);
        ft_display_name = "Reserved";
    } else {
        ft_display_name = val64_to_str_const(frame_type, http3_frame_types, "Unknown");
        col_append_sep_str(pinfo->cinfo, COL_INFO, ", ", ft_display_name);
    }
    proto_tree_add_item_ret_varint(ft_tree, hf_http3_frame_length, tvb, offset, -1, ENC_VARINT_QUIC, &frame_length,
                                   &lenvar);
    proto_item_set_text(ti_ft, "%s len=%d", ft_display_name, lenvar);
    offset += lenvar;

    if (frame_length) {
        proto_tree_add_item(tree, hf_http3_frame_payload, tvb, offset, (int)frame_length, ENC_NA);

        switch (frame_type) {
        case HTTP3_DATA: { /* TODO: dissect Data Frame */
            tvbuff_t *next_tvb = tvb_new_subset_length(tvb, offset, (int)frame_length);
            dissect_http3_data(next_tvb, pinfo, ft_tree, 0, http3_stream);
        } break;
        case HTTP3_HEADERS: { /* TODO: dissect Headers Frame */
#ifdef HAVE_NGHTTP3
            tvbuff_t *next_tvb = tvb_new_subset_length(tvb, offset, (int)frame_length);
            dissect_http3_headers(next_tvb, pinfo, ft_tree, 0, offset, http3_stream);
#endif /* HAVE_NGHTTP3 */
        } break;
        case HTTP3_CANCEL_PUSH: /* TODO: dissect Cancel_Push Frame */
            break;
        case HTTP3_SETTINGS: { /* Settings Frame */
            tvbuff_t *next_tvb = tvb_new_subset_length(tvb, offset, (int)frame_length);
            dissect_http3_settings(next_tvb, pinfo, ft_tree, 0);
        } break;
        case HTTP3_PUSH_PROMISE: /* TODO: dissect Push_Promise_Frame */
            break;
        case HTTP3_GOAWAY: /* TODO: dissect Goaway Frame */
            break;
        case HTTP3_MAX_PUSH_ID: /* TODO: dissect Max_Push_ID Frame */
            break;
        case HTTP3_PRIORITY_UPDATE_REQUEST_STREAM:
        case HTTP3_PRIORITY_UPDATE_PUSH_STREAM: { /* Priority_Update Frame */
            tvbuff_t *next_tvb = tvb_new_subset_length(tvb, offset, (int)frame_length);
            dissect_http3_priority_update(next_tvb, pinfo, ft_tree, 0, frame_length);
        } break;
        default: /* TODO: add expert_advise */
            break;
        }

        offset += (int)frame_length;
    }

    return offset;
}

static void
report_unknown_stream_type(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, int offset,
                           quic_stream_info *stream_info, http3_stream_info_t *http3_stream)
{
    /*
     * https://www.rfc-editor.org/rfc/rfc9114.html#name-unidirectional-streams
     *
     * "If the stream header indicates a stream type which is not supported by
     * the recipient, the remainder of the stream cannot be consumed as the
     * semantics are unknown."
     */
    proto_tree_add_expert_format(tree, pinfo, &ei_http3_unknown_stream_type, tvb, offset, 0,
                                 "Unknown stream type %#" PRIx64 " on Stream ID %#" PRIx64,
                                 http3_stream->uni_stream_type, stream_info->stream_id);
}

/**
 * https://www.rfc-editor.org/rfc/rfc7541#section-5.1
 * via
 * https://www.rfc-editor.org/rfc/rfc9204.html#name-prefixed-integers
 *
 * Read a QPACK varint value, return number of consumed bytes, including the prefix byte.
 *
 * Optionally return the value of the one-bit flag that precedes the QPACK prefixed integer.
 *
 * Such flag is is interpreted differently, depending on the context:
 * - If the prefixed integer represents length of a string literal, the flag value
 *   indicates that the following string literal is encoded using Huffman code.
 *   See https://www.rfc-editor.org/rfc/rfc7541#section-5.2 for details.
 * - If the prefixed integer represents a name index, the flag value indicates
 *   that the following name index belongs to the static/dynamic table.
 *   See https://www.rfc-editor.org/rfc/rfc9204.html#name-insert-with-name-reference
 *   for details.
 */
#define HTTP3_QPACK_MAX_SHIFT 62
#define HTTP3_QPACK_MAX_INT ((1ull << HTTP3_QPACK_MAX_SHIFT) - 1)

static gint
read_qpack_prefixed_integer(guint8 *buf, guint8 *end, gint prefix,
                            guint64 *out_result, gboolean *out_fin, gboolean *out_flag)
{
    guint64       k     = (uint8_t)((1 << prefix) - 1);
    guint64       n     = 0;
    guint64       add   = 0;
    guint64       shift = 0;
    const guint8 *p     = buf;

    if (((*p) & k) != k) {
        *out_result = (*p) & k;
        *out_fin    = true;
        return 1;
    }

    if (out_flag) {
        *out_flag = *p & (1 << prefix);
    }

    n = k;

    if (++p == end) {
        *out_result = n;
        *out_fin    = false;
        return (gint)(p - buf);
    }

    for (; p != end; ++p, shift += 7) {
        add = (*p) & 0x7f;
        if (shift > HTTP3_QPACK_MAX_SHIFT) {
            return -1;
        }
        if ((HTTP3_QPACK_MAX_INT >> shift) < add) {
            return -1;
        }
        add <<= shift;
        if (HTTP3_QPACK_MAX_INT - add < n) {
            return -1;
        }

        n += add;

        if (((*p) & (1 << 7)) == 0) {
            break;
        }
    }

    *out_result = n;

    /* If we consumed all bytes, return the consumed bytes */
    if (p == end) {
        *out_fin = false;
        return (gint)(p - buf);
    }

    /* Otherwise, consume extra byte and mark the fin output param */
    if (out_fin) {
        *out_fin = true;
    }
    return (gint)(p + 1 - buf);
}

static gint
dissect_http3_qpack_encoder_stream(tvbuff_t *tvb, packet_info *pinfo _U_, proto_tree *tree, int offset,
                                   uint8_t *qpack_buf, guint remaining, http3_stream_info_t *http3_stream _U_)
{
    guint       remaining_captured;
    proto_item  *opcode_ti;
    proto_tree  *opcode_tree;
    guint       decoded = 0;
    gint        fin = 0, inc = 0;
    guint       can_continue = 1;
    uint8_t     *end = qpack_buf + remaining;

    remaining_captured = tvb_captured_length_remaining(tvb, offset);
    DISSECTOR_ASSERT(remaining_captured == remaining);

    if (qpack_buf == NULL || remaining <= 0) {
        HTTP3_DISSECTOR_DPRINTF("Exiting qpack_buf=%p remaining=%d", qpack_buf, remaining);
        return 0;
    }

    while (decoded < remaining && can_continue) {
        gint   opcode_offset = 0;
        gint   opcode_len    = 0;
        guint8 opcode        = 0;
        fin                  = 0;

        opcode = qpack_buf[decoded] & QPACK_OPCODE_MASK;

        HTTP3_DISSECTOR_DPRINTF("Decoding opcode=%" PRIu8 " decoded=%d remaining=%d", opcode, decoded, remaining);

        if (opcode & QPACK_OPCODE_INSERT_INDEXED) {
            gint     table_entry_len  = 0;
            guint64  table_entry      = 0;
            gint     value_offset     = 0;
            gint     value_len        = 0;
            gint     val_bytes_offset = 0;
            guint64  val_bytes_len    = 0;
            gboolean value_huffman    = FALSE;

            /*
             *   0   1   2   3   4   5   6   7
             * +---+---+---+---+---+---+---+---+
             * | 1 | T |    Name Index (6+)    |
             * +---+---+-----------------------+
             * | H |     Value Length (7+)     |
             * +---+---------------------------+
             * |  Value String (Length bytes)  |
             * +-------------------------------+
             *
             */
            opcode_offset = offset + decoded;

            decoded += read_qpack_prefixed_integer(qpack_buf + decoded, end, 6, &table_entry, &fin, NULL);
            table_entry_len = offset + decoded - opcode_offset;

            value_offset = offset + decoded - opcode_offset;
            decoded += read_qpack_prefixed_integer(qpack_buf + decoded, end, 7, &val_bytes_len, &fin, &value_huffman);
            val_bytes_offset = offset + decoded - opcode_offset;

            decoded += (guint32)val_bytes_len;
            value_len = opcode + decoded - value_offset;

            opcode_len = offset + decoded - opcode_offset;

            opcode_ti   = proto_tree_add_item(tree, hf_http3_qpack_encoder_opcode_insert_indexed, tvb, opcode_offset,
                                            opcode_len, ENC_NA);
            opcode_tree = proto_item_add_subtree(opcode_ti, ett_http3_qpack_opcode);
            proto_tree_add_item(opcode_tree, hf_http3_qpack_encoder_opcode_insert_indexed_ref, tvb, opcode_offset,
                                table_entry_len, ENC_NA);
            if (value_huffman) {
                proto_tree_add_item(opcode_tree, hf_http3_qpack_encoder_opcode_insert_indexed_hval, tvb,
                                    val_bytes_offset, (guint32)val_bytes_len, ENC_NA);
            } else {
                proto_tree_add_item(opcode_tree, hf_http3_qpack_encoder_opcode_insert_indexed_val, tvb,
                                    val_bytes_offset, (guint32)val_bytes_len, ENC_NA);
            }
            proto_item_set_text(opcode_ti, "QPACK encoder INSERT_INDEXED ref_len=%d ref=%" PRIu64 " val_len=%d",
                                table_entry_len, table_entry, value_len);
        } else if (opcode & QPACK_OPCODE_INSERT) {
            guint    name_len_offset    = 0;
            guint    name_len_len       = 0;
            guint    name_len           = 0;
            gboolean name_huffman       = FALSE;
            guint    name_bytes_offset  = 0;
            guint64  name_bytes_len     = 0;
            guint    val_len_offset     = 0;
            guint    val_len_len        = 0;
            guint    val_len            = 0;
            gboolean value_huffman      = FALSE;
            guint    val_bytes_offset   = 0;
            guint64  val_bytes_len      = 0;

            /*
             *  Insert with literal name:
             *  See https://datatracker.ietf.org/doc/html/rfc9204#name-insert-with-literal-name
             *
             *   0   1   2   3   4   5   6   7
             * +---+---+---+---+---+---+---+---+
             * | 0 | 1 | H | Name Length (5+)  |
             * +---+---+---+-------------------+
             * |  Name String (Length bytes)   |
             * +---+---------------------------+
             * | H |     Value Length (7+)     |
             * +---+---------------------------+
             * |  Value String (Length bytes)  |
             * +-------------------------------+
             *
             */

            /* Read the 5-encoded name length */
            name_len_offset = opcode_offset = offset + decoded;
            decoded += read_qpack_prefixed_integer(qpack_buf + decoded, end, 5, &name_bytes_len, &fin, &name_huffman);
            name_len_len      = offset + decoded - name_len_offset;
            name_len          = name_len_len + (guint32)name_bytes_len;
            name_bytes_offset = offset + decoded - opcode_offset;
            decoded += (guint32)name_bytes_len;

            /* Read the 7-encoded value length */
            val_len_offset = offset + decoded - opcode_offset;
            decoded += read_qpack_prefixed_integer(qpack_buf + decoded, end, 7, &val_bytes_len, &fin, &value_huffman);
            val_len_len      = offset + decoded - val_len_offset;
            val_len          = val_len_len + (guint32)val_bytes_len;
            val_bytes_offset = offset + decoded - opcode_offset;

            decoded += (guint32)val_bytes_len;

            opcode_len = offset + decoded - opcode_offset;
            opcode_ti =
                proto_tree_add_item(tree, hf_http3_qpack_encoder_opcode_insert, tvb, opcode_offset, opcode_len, ENC_NA);
            opcode_tree = proto_item_add_subtree(opcode_ti, ett_http3_qpack_opcode);
            if (name_huffman) {
                proto_tree_add_item(opcode_tree, hf_http3_qpack_encoder_opcode_insert_hname, tvb, name_bytes_offset,
                                    (guint32)name_bytes_len, ENC_NA);
            } else {
                proto_tree_add_item(opcode_tree, hf_http3_qpack_encoder_opcode_insert_name, tvb, name_bytes_offset,
                                    (guint32)name_bytes_len, ENC_NA);
            }

            if (value_huffman) {
                proto_tree_add_item(opcode_tree, hf_http3_qpack_encoder_opcode_insert_hval, tvb, val_bytes_offset,
                                    (guint32)val_bytes_len, ENC_NA);
            } else {
                proto_tree_add_item(opcode_tree, hf_http3_qpack_encoder_opcode_insert_val, tvb, val_bytes_offset,
                                    (guint32)val_bytes_len, ENC_NA);
            }
            proto_item_set_text(opcode_ti, "QPACK encoder opcode: INSERT name_len=%d val_len=%d", name_len, val_len);
        } else if (opcode & QPACK_OPCODE_SET_DTABLE_CAP) {
            guint64 dynamic_capacity = 0;

            /*
             *   0   1   2   3   4   5   6   7
             * +---+---+---+---+---+---+---+---+
             * | 0 | 0 | 1 |   Capacity (5+)   |
             * +---+---+---+-------------------+
             */

            opcode_offset = offset + decoded;

            decoded += read_qpack_prefixed_integer(qpack_buf + decoded, end, 5, &dynamic_capacity, &fin, NULL);
            opcode_len = offset + decoded - opcode_offset;

            opcode_ti   = proto_tree_add_item(tree, hf_http3_qpack_encoder_opcode_dtable_cap, tvb, opcode_offset,
                                            opcode_len, ENC_NA);
            opcode_tree = proto_item_add_subtree(opcode_ti, ett_http3_qpack_opcode);
            proto_tree_add_uint64(opcode_tree, hf_http3_qpack_encoder_opcode_dtable_cap_val, tvb, opcode_offset, opcode_len,
                                  dynamic_capacity);
            proto_item_set_text(opcode_ti, "QPACK encoder opcode: Set DTable Cap=%" PRIu64 "", dynamic_capacity);
        } else if (opcode == QPACK_OPCODE_DUPLICATE) {
            guint64 duplicate_of = 0;

            /*
             *   0   1   2   3   4   5   6   7
             * +---+---+---+---+---+---+---+---+
             * | 0 | 0 | 0 |    Index (5+)     |
             * +---+---+---+-------------------+
             */

            opcode_offset        = offset + decoded;

            inc = read_qpack_prefixed_integer(qpack_buf + decoded, end, 5, &duplicate_of, &fin, NULL);
            DISSECTOR_ASSERT(0 < inc);
            DISSECTOR_ASSERT(decoded + inc <= remaining);
            decoded += inc;

            opcode_len = offset + decoded - opcode_offset;
            proto_tree_add_item(tree, hf_http3_qpack_encoder_opcode_duplicate, tvb, opcode_offset,
                                opcode_len, ENC_NA);
        } else {
            HTTP3_DISSECTOR_DPRINTF("Opcode=%" PRIu8 ": UNKNOWN", opcode);
            can_continue = 0;
        }
    }

    return offset + decoded;
}

static gint
dissect_http3_qpack_enc(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, int offset,
                        http3_stream_info_t *http3_stream)
{
    gint                  remaining, remaining_captured, retval, decoded = 0;
    proto_item *          qpack_update;
    proto_tree *          qpack_update_tree;
    http3_session_info_t *http3_session;
    uint8_t *             qpack_buf = NULL;

    remaining_captured = tvb_captured_length_remaining(tvb, offset);
    remaining          = tvb_reported_length_remaining(tvb, offset);
    DISSECTOR_ASSERT(remaining_captured == remaining);
    retval = remaining;

    http3_session = http3_session_lookup_or_create(pinfo);
    DISSECTOR_ASSERT(http3_session);

    if (remaining > 0) {
        qpack_buf = (uint8_t *)tvb_memdup(wmem_packet_scope(), tvb, offset, remaining);
    }

    /*
     * Add a QPACK encoder tree item.
     */
    qpack_update      = proto_tree_add_item(tree, hf_http3_qpack_encoder, tvb, offset, remaining, ENC_NA);
    qpack_update_tree = proto_item_add_subtree(qpack_update, ett_http3_qpack_update);
    decoded           = dissect_http3_qpack_encoder_stream(tvb, pinfo, qpack_update_tree, offset, qpack_buf,
                                                           (guint)remaining, http3_stream);

#ifdef HAVE_NGHTTP3
    if (qpack_buf && 0 < remaining) {
        gint                   qpack_buf_len = decoded - offset, nread;
        proto_item *           ti;
        http3_stream_dir       packet_direction = http3_packet_get_direction(http3_stream);
        nghttp3_qpack_decoder *decoder          = http3_session->qpack_decoder[packet_direction];
        guint32                icnt_before, icnt_after;

        /*
         * Get the instr count prior to processing the data.
         */
        icnt_after = icnt_before = (guint32)nghttp3_qpack_decoder_get_icnt(decoder);

        HTTP3_DISSECTOR_DPRINTF("decode encoder stream: decoder=%p remaining=%u", decoder, remaining);

        nread = (gint)nghttp3_qpack_decoder_read_encoder(decoder, qpack_buf, qpack_buf_len);
        if (nread < 0) {
            quic_cid_t quic_cid          = {.len = 0};
            gboolean   initial_cid_found = quic_conn_data_get_conn_client_dcid_initial(pinfo, &quic_cid);
            proto_tree_add_expert_format(
                tree, pinfo, &ei_http3_qpack_failed, tvb, offset, 0, "QPAC decoder %p DCID %s [found=%d] error %d (%s)",
                decoder, cid_to_string(&quic_cid), initial_cid_found, nread, nghttp3_strerror((int)nread));
        }

        icnt_after = (guint32)nghttp3_qpack_decoder_get_icnt(decoder);
        proto_item_set_text(qpack_update, "QPACK encoder stream; %d opcodes (%d total)", icnt_after - icnt_before,
                            icnt_after);

        ti = proto_tree_add_uint(qpack_update_tree, hf_http3_qpack_encoder_icnt, tvb, offset, 0, icnt_after);
        proto_item_set_generated(ti);

        ti = proto_tree_add_uint(qpack_update_tree, hf_http3_qpack_encoder_icnt_inc, tvb, offset, 0,
                                 icnt_after - icnt_before);
        proto_item_set_generated(ti);
    }
#else
    (void)qpack_buf;
    (void)qpack_update;
    (void)decoded;
#endif /* HAVE_NGHTTP3 */

    return retval;
}

static int
dissect_http3_client_bidi_stream(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, int offset, quic_stream_info *stream_info _U_,
                         http3_stream_info_t *http3_stream)
{
    proto_item *ti_stream;
    proto_tree *stream_tree;

    ti_stream = proto_tree_add_item(tree, hf_http3_stream_bidi, tvb, offset, 1, ENC_NA);
    stream_tree = proto_item_add_subtree(ti_stream, ett_http3_stream_bidi);

    while (tvb_reported_length_remaining(tvb, offset)) {
        if (!http3_check_frame_size(tvb, pinfo, offset)) {
            return tvb_captured_length(tvb);
        }
        offset = dissect_http3_frame(tvb, pinfo, stream_tree, offset, http3_stream);
    }

    return offset;
}

static int
dissect_http3_uni_stream(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, int offset, quic_stream_info *stream_info,
                         http3_stream_info_t *http3_stream)
{
    guint64     stream_type;
    int         lenvar;
    proto_item *ti_stream, *ti_stream_type;
    proto_tree *stream_tree;
    const gchar *stream_display_name;

    ti_stream = proto_tree_add_item(tree, hf_http3_stream_uni, tvb, offset, -1, ENC_NA);
    stream_tree = proto_item_add_subtree(ti_stream, ett_http3_stream_uni);

    if (stream_info->offset == 0) {
        ti_stream_type = proto_tree_add_item_ret_varint(stream_tree, hf_http3_stream_uni_type, tvb, offset, -1, ENC_VARINT_QUIC, &stream_type,
                                            &lenvar);
        offset += lenvar;
        http3_stream->uni_stream_type = stream_type;
        if (http3_is_reserved_code(stream_type)) {
            // Reserved to exercise requirement that unknown types are ignored.
            proto_item_set_text(ti_stream_type, "Stream Type: Reserved (%#" PRIx64 ")", stream_type);
            stream_display_name = "Reserved";
        }
        else {
            stream_display_name = val64_to_str_const(stream_type, http3_stream_types, "Unknown");
        }
        proto_item_set_text(ti_stream, "UNI STREAM: %s off=%" PRIu64 "", stream_display_name, stream_info->stream_offset);
    } else {
        stream_type = http3_stream->uni_stream_type;
        /*ti_stream_type = proto_tree_add_item(stream_tree, hf_http3_stream_uni_type, tvb, offset, -1, ENC_NA);*/
    }

    switch (stream_type) {
    case HTTP3_STREAM_TYPE_CONTROL:
        break;
    case HTTP3_STREAM_TYPE_PUSH:
        // The remaining data of this stream consists of HTTP/3 frames.
        if (stream_info->offset == 0) {
            proto_tree_add_item_ret_varint(stream_tree, hf_http3_push_id, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &lenvar);
            offset += lenvar;
        }
        break;
    case HTTP3_STREAM_TYPE_QPACK_ENCODER:
        offset = dissect_http3_qpack_enc(tvb, pinfo, stream_tree, offset, http3_stream);
        break;
    case HTTP3_STREAM_TYPE_QPACK_DECODER:
        // TODO
        offset = tvb_captured_length(tvb);
        break;
    default:
        // Unknown or reserved stream type, consume everything.
        if (!http3_is_reserved_code(stream_type)) {
            if (!PINFO_FD_VISITED(pinfo)) {
                http3_stream->broken_from_offset = stream_info->offset + offset;
            }
            report_unknown_stream_type(tvb, pinfo, stream_tree, offset, stream_info, http3_stream);
        }
        offset = tvb_captured_length(tvb);
        break;
    }

    return offset;
}

static int
dissect_http3(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    quic_stream_info *   stream_info = (quic_stream_info *)data;
    proto_item *         ti;
    proto_tree *         http3_tree;
    int                  offset = 0;
    http3_stream_info_t *http3_stream;

    if (!stream_info) {
        return 0;
    }

    switch (QUIC_STREAM_TYPE(stream_info->stream_id)) {
    case QUIC_STREAM_CLIENT_BIDI:
        /* Used for HTTP requests and responses. */
        if (!http3_check_frame_size(tvb, pinfo, offset)) {
            return tvb_captured_length(tvb);
        }
        break;
    case QUIC_STREAM_SERVER_BIDI:
        /* "HTTP/3 does not use server-initiated bidirectional streams,
         * though an extension could define a use for these streams." */
        break;
    case QUIC_STREAM_CLIENT_UNI:
    case QUIC_STREAM_SERVER_UNI:
        break;
    }

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "HTTP3");
    // Only clear the columns if this is the first HTTP/3 STREAM in the packet.
    if (!proto_is_frame_protocol(pinfo->layers, "http3")) {
        col_clear(pinfo->cinfo, COL_INFO);
    }

    ti         = proto_tree_add_item(tree, proto_http3, tvb, 0, -1, ENC_NA);
    http3_tree = proto_item_add_subtree(ti, ett_http3);

    http3_stream = (http3_stream_info_t *)quic_stream_get_proto_data(pinfo, stream_info);
    if (!http3_stream) {
        http3_stream = wmem_new0(wmem_file_scope(), http3_stream_info_t);
        quic_stream_add_proto_data(pinfo, stream_info, http3_stream);
        http3_stream->quic_stream_info = stream_info;
        http3_stream->id               = stream_info->stream_id;
    }

    // If a STREAM has unknown data, everything afterwards cannot be dissected.
    if (http3_stream->broken_from_offset && http3_stream->broken_from_offset <= stream_info->offset + offset) {
        report_unknown_stream_type(tvb, pinfo, tree, offset, stream_info, http3_stream);
        return tvb_captured_length(tvb);
    }

    switch (QUIC_STREAM_TYPE(stream_info->stream_id)) {
    case QUIC_STREAM_CLIENT_BIDI:
        /* Used for HTTP requests and responses. */
        dissect_http3_client_bidi_stream(tvb, pinfo, http3_tree, offset, stream_info, http3_stream);
        break;

    case QUIC_STREAM_SERVER_BIDI:
        /* "HTTP/3 does not use server-initiated bidirectional streams,
         * though an extension could define a use for these streams." */
        // XXX expert info?
        return tvb_captured_length(tvb);

    case QUIC_STREAM_CLIENT_UNI:
    case QUIC_STREAM_SERVER_UNI:
        dissect_http3_uni_stream(tvb, pinfo, http3_tree, offset, stream_info, http3_stream);
        break;
    }

    return tvb_captured_length(tvb);
}

#ifdef HAVE_NGHTTP3
static void
register_static_headers(void)
{
    header_fields_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /*
     * Here hf[x].hfinfo.name is a header method which is used as key
     * for matching ids while processing http3 packets.
     */
    static hf_register_info hf[] = {
        { &hf_http3_headers_authority,
          { ":authority", "http3.headers.authority",
             FT_STRING, BASE_NONE, NULL, 0x0,
            "Authority portion of the target URI", HFILL }
        },
        { &hf_http3_headers_status,
          { ":status", "http3.headers.status",
            FT_UINT16, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_http3_headers_path,
          { ":path", "http3.headers.path",
            FT_STRING, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_http3_headers_method,
          { ":method", "http3.headers.method",
            FT_STRING, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_http3_headers_scheme,
          { ":scheme", "http3.headers.scheme",
            FT_STRING, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_http3_headers_accept,
          { "accept", "http3.headers.accept",
             FT_STRING, BASE_NONE, NULL, 0x0,
            "Media types that are acceptable to the user agent", HFILL }
        },
        { &hf_http3_headers_accept_charset,
          { "accept-charset", "http3.headers.accept_charset",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Acceptable charsets in textual responses for the user agent", HFILL }
        },
        { &hf_http3_headers_accept_encoding,
          { "accept-encoding", "http3.headers.accept_encoding",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Acceptable content codings (like compression) in responses for the user agent", HFILL }
        },
        { &hf_http3_headers_accept_language,
          { "accept-language", "http3.headers.accept_language",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Preferred natural languages for the user agent", HFILL }
        },
        { &hf_http3_headers_accept_ranges,
          { "accept-ranges", "http3.headers.accept_ranges",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Bytes range which server may use for partial data transfer", HFILL }
        },
        { &hf_http3_headers_access_control_allow_origin,
          { "access-control-allow-origin", "http3.headers.access_control_allow_origin",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Origin control for cross-origin resource sharing", HFILL }
        },
        { &hf_http3_headers_age,
          { "age", "http3.headers.age",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Time in seconds which was spent for transferring data through proxy", HFILL }
        },
        { &hf_http3_headers_allow,
          { "allow", "http3.headers.allow",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "List of allowed methods for request", HFILL }
        },
        { &hf_http3_headers_authorization,
          { "authorization", "http3.headers.authorization",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Credentials for a server-side authorization", HFILL }
        },
        { &hf_http3_headers_cache_control,
          { "cache-control", "http3.headers.cache_control",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Request or response directives for a cache control", HFILL }
        },
        { &hf_http3_headers_content_disposition,
          { "content-disposition", "http3.headers.content_disposition",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Indicates that response will be displayed as page or downloaded with dialog box", HFILL }
        },
        { &hf_http3_headers_content_encoding,
          { "content-encoding", "http3.headers.content_encoding",
            FT_STRING, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_http3_headers_content_language,
          { "content-language", "http3.headers.content_language",
            FT_STRING, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_http3_headers_content_length,
          { "content-length", "http3.headers.content_length",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Size of body in bytes", HFILL }
        },
        { &hf_http3_headers_content_location,
          { "content-location", "http3.headers.content_location",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Alternative URL for a response data", HFILL }
        },
        { &hf_http3_headers_content_range,
          { "content-range", "http3.headers.content_range",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Range of bytes which was sent by server for partial data transfer", HFILL }
        },
        { &hf_http3_headers_content_type,
          { "content-type", "http3.headers.content_type",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "MIME type of response", HFILL }
        },
        { &hf_http3_headers_cookie,
          { "cookie", "http3.headers.cookie",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Stored cookies", HFILL }
        },
        { &hf_http3_headers_date,
          { "date", "http3.headers.date",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Date and time at which the data was originated", HFILL }
        },
        { &hf_http3_headers_etag,
          { "etag", "http3.headers.etag",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Directive for version indication of resource", HFILL }
        },
        { &hf_http3_headers_expect,
          { "expect", "http3.headers.expect",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Expectations that need to be fulfilled for correct request", HFILL }
        },
        { &hf_http3_headers_expires,
          { "expires", "http3.headers.expires",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Data after which resource will be stale", HFILL }
        },
        { &hf_http3_headers_from,
          { "from", "http3.headers.from",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Email of a person who responsible for a requesting data", HFILL }
        },
        { &hf_http3_headers_if_match,
          { "if-match", "http3.headers.if_match",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Mechanism for requesting data matched by a list of ETags", HFILL }
        },
        { &hf_http3_headers_if_modified_since,
          { "if-modified-since", "http3.headers.if_modified_since",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Resource will be sent with status code 200 if it was modified otherwise with status code 304", HFILL }
        },
        { &hf_http3_headers_if_none_match,
          { "if-none-match", "http3.headers.if_none_match",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Mechanism for requesting data not matched by a list of ETags", HFILL }
        },
        { &hf_http3_headers_if_range,
          { "if-range", "http3.headers.if_range",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Mechanism for a range request which is used to check if a resource was modified", HFILL }
        },
        { &hf_http3_headers_if_unmodified_since,
          { "if-unmodified-since", "http3.headers.if_unmodified_since",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Resource will be processed if it was not modified otherwise 412 error will be returned", HFILL }
        },
        { &hf_http3_headers_last_modified,
          { "last-modified", "http3.headers.last_modified",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Date and time at which the origin server believes the resource was last modified", HFILL }
        },
        { &hf_http3_headers_link,
          { "link", "http3.headers.link",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Mechanism for indicating that resource will be preloaded", HFILL }
        },
        { &hf_http3_headers_location,
          { "location", "http3.headers.location",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Mechanism for indicating that client will be redirected", HFILL }
        },
        { &hf_http3_headers_max_forwards,
          { "max-forwards", "http3.headers.max_forwards",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Mechanism for limiting the number of proxies", HFILL }
        },
        { &hf_http3_headers_proxy_authenticate,
          { "proxy-authenticate", "http3.headers.proxy_authenticate",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Authentication method that should be used to gain access to a resource behind a proxy server", HFILL }
        },
        { &hf_http3_headers_proxy_authorization,
          { "proxy-authorization", "http3.headers.proxy_authorization",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Credentials for a proxy-side authorization", HFILL }
        },
        { &hf_http3_headers_range,
          { "range", "http3.headers.range",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Range of resource bytes that server should return", HFILL }
        },
        { &hf_http3_headers_referer,
          { "referer", "http3.headers.referer",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Address of the previous web page", HFILL }
        },
        { &hf_http3_headers_refresh,
          { "refresh", "http3.headers.refresh",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Time in seconds after which client will be redirected by given url", HFILL }
        },
        { &hf_http3_headers_retry_after,
          { "retry-after", "http3.headers.retry_after",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Mechanism to indicate when resource expected to be available", HFILL }
        },
        { &hf_http3_headers_server,
          { "server", "http3.headers.server",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Information about server software", HFILL }
        },
        { &hf_http3_headers_set_cookie,
          { "set-cookie", "http3.headers.set_cookie",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Send a cookie to the client", HFILL }
        },
        { &hf_http3_headers_strict_transport_security,
          { "strict-transport-security", "http3.headers.strict_transport_security",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "HSTS indicates that resource should be accessed only using HTTPS", HFILL }
        },
        { &hf_http3_headers_user_agent,
          { "user-agent", "http3.headers.user_agent",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Information about client software", HFILL }
        },
        { &hf_http3_headers_vary,
          { "vary", "http3.headers.vary",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Mechanism for selecting which header will be used for content negotiation algorithm", HFILL }
        },
        { &hf_http3_headers_via,
          { "via", "http3.headers.via",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Additional information for loop detection and protocol capabilities in proxy requests", HFILL }
        },
        { &hf_http3_headers_www_authenticate,
          { "www-authenticate", "http3.headers.www_authenticate",
            FT_STRING, BASE_NONE, NULL, 0x0,
            "Authentication method that should be used to gain access to a resource", HFILL }
        }
    };

    gchar *header_name;
    for (guint i = 0; i < G_N_ELEMENTS(hf); ++i) {
        header_name = g_strdup(hf[i].hfinfo.name);

        g_hash_table_insert(header_fields_hash, header_name, &hf[i].hfinfo.id);
    }
    proto_register_field_array(proto_http3, hf, G_N_ELEMENTS(hf));
}
#endif /* HAVE_NGHTTP3 */

void
proto_register_http3(void)
{
    expert_module_t *      expert_http3;
    module_t *module_http3 _U_;

    static hf_register_info hf[] = {
        { &hf_http3_stream_uni,
          { "Uni Stream", "http3.stream.uni",
             FT_NONE, BASE_NONE, NULL, 0x0,
             NULL, HFILL }
        },
        { &hf_http3_stream_uni_type,
          { "Uni Stream Type", "http3.stream_uni_type",
            FT_UINT64, BASE_HEX|BASE_VAL64_STRING, VALS64(http3_stream_types), 0x0,
            NULL, HFILL }
        },
        { &hf_http3_stream_bidi,
          { "Request Stream", "http3.stream",
             FT_NONE, BASE_NONE, NULL, 0x0,
             NULL, HFILL }
        },
        { &hf_http3_push_id,
          { "Push ID", "http3.push_id",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_http3_frame,
          { "Frame", "http3.frame",
            FT_NONE, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_http3_frame_type,
          { "Type", "http3.frame_type",
            FT_UINT64, BASE_HEX|BASE_VAL64_STRING, VALS64(http3_frame_types), 0x0,
            "Frame Type", HFILL }
        },
        { &hf_http3_frame_length,
          { "Length", "http3.frame_length",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Length of the Frame Payload", HFILL }
        },
        { &hf_http3_frame_payload,
          { "Frame Payload", "http3.frame_payload",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        /* Data */
        { &hf_http3_data,
          { "Data", "http3.data",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        /* Headers */
        //{ &hf_http3_headers,
        //     { "Header", "http3.headers",
        //        FT_UINT32, BASE_DEC, NULL, 0x0,
        //        NULL, HFILL }
        //},
        { &hf_http3_headers_count,
             { "Headers Count", "http3.headers.count",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL }
        },
        { &hf_http3_header,
             { "Header", "http3.headers.header",
                FT_NONE, BASE_NONE, NULL, 0x0,
                NULL, HFILL }
        },
        { &hf_http3_header_length,
             { "Header Length", "http3.headers.header.length",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL }
        },
        { &hf_http3_header_name_length,
             { "Name Length", "http3.headers.header.name.length",
               FT_UINT32, BASE_DEC, NULL, 0x0,
               NULL, HFILL }
        },
        { &hf_http3_header_name,
             { "Name", "http3.header.header.name",
               FT_STRING, BASE_NONE, NULL, 0x0,
                NULL, HFILL }
        },
        { &hf_http3_header_value_length,
            { "Value Length", "http3.headers.header.value.length",
              FT_UINT32, BASE_DEC, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_header_value,
            { "Value", "http3.headers.header.value",
                FT_STRING, BASE_NONE, NULL, 0x0,
                NULL, HFILL }
        },
        { &hf_http3_header_request_full_uri,
            { "Full request URI", "http3.request.full_uri",
              FT_STRING, BASE_NONE, NULL, 0x0,
              "The full requested URI (including host name)", HFILL }
        },
        { &hf_http3_header_qpack_blocked,
            { "HEADERS head-of-line-blocked on QPACK encoder stream", "http3.header.qpack.blocked",
                FT_BOOLEAN, BASE_NONE, NULL, 0x0,
                NULL, HFILL }
        },
        { &hf_http3_header_qpack_blocked_stream_rcint,
            { "Required instruction count", "http3.header.qpack.blocked.rcint",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL }
        },
       { &hf_http3_header_qpack_blocked_decoder_wicnt,
            { "Available instruction count", "http3.header.qpack.blocked.wcint",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL }
        },
        //{ &hf_http3_header_qpack_fatal,
        //    { "QPACK decoding error", "http3.header.qpack.fatal",
        //        FT_BOOLEAN, BASE_NONE, NULL, 0x0,
        //        NULL, HFILL }
        //},
        //{ &hf_http3_qpack,
        //    { "QPACK", "http3.qpack",
        //        FT_BYTES, BASE_NONE, NULL, 0x0,
        //        NULL, HFILL }
        //},
        { &hf_http3_qpack_encoder,
            { "QPACK encoder", "http3.qpack.encoder",
                FT_BYTES, BASE_NONE, NULL, 0x0,
                NULL, HFILL }
        },
        //{ &hf_http3_qpack_encoder_length,
        //    { "QPACK encoder update length", "http3.qpack.encoder.length",
        //        FT_UINT32, BASE_DEC, NULL, 0x0,
        //        NULL, HFILL }
        //},
        { &hf_http3_qpack_encoder_icnt,
            { "QPACK encoder instruction count", "http3.qpack.encoder.icnt",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL }
        },
        { &hf_http3_qpack_encoder_icnt_inc,
            { "QPACK encoder instruction count increment", "http3.qpack.encoder.icnt.inc",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL }
        },
       //{ &hf_http3_qpack_encoder_opcode,
       //     { "QPACK encoder opcode", "http3.qpack.encoder.opcode",
       //       FT_BYTES, BASE_NONE, NULL, 0x0,
       //       NULL, HFILL }
       // },
        { &hf_http3_qpack_encoder_opcode_insert_indexed,
            { "Insert with Name Reference", "http3.qpack.encoder.opcode.insert_indexed",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_qpack_encoder_opcode_insert_indexed_ref,
            { "Name Reference", "http3.qpack.encoder.opcode.insert_indexed.ref",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_qpack_encoder_opcode_insert_indexed_val,
            { "Value", "http3.qpack.encoder.opcode.insert_indexed.val",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_qpack_encoder_opcode_insert_indexed_hval,
            { "Value (Huffman)", "http3.qpack.encoder.opcode.insert_indexed.hval",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_qpack_encoder_opcode_insert,
            { "Insert with Literal Name", "http3.qpack.encoder.opcode.insert",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_qpack_encoder_opcode_insert_name,
            { "Literal Name", "http3.qpack.encoder.opcode.insert.name",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_qpack_encoder_opcode_insert_hname,
            { "Literal Name (Huffman)", "http3.qpack.encoder.opcode.insert.hname",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_qpack_encoder_opcode_insert_val,
            { "Value", "http3.qpack.encoder.opcode.insert.val",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_qpack_encoder_opcode_insert_hval,
            { "Value (Huffman)", "http3.qpack.encoder.opcode.insert.hval",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_qpack_encoder_opcode_duplicate,
            { "Duplicate", "http3.qpack.encoder.opcode.duplicate",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
        //{ &hf_http3_qpack_encoder_opcode_duplicate_val,
        //    { "Duplicate Index", "http3.qpack.encoder.opcode.duplicate.val",
        //      FT_BYTES, BASE_NONE, NULL, 0x0,
        //      NULL, HFILL }
        //},
        { &hf_http3_qpack_encoder_opcode_dtable_cap,
            { "Set Dynamic Table Capacity", "http3.qpack.encoder.opcode.dtable_cap",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_qpack_encoder_opcode_dtable_cap_val,
            { "Capacity", "http3.qpack.encoder.opcode.dtable_cap.val",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              NULL, HFILL }
        },

        /* Settings */
        { &hf_http3_settings,
            { "Settings", "http3.settings",
               FT_NONE, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_settings_identifier,
            { "Settings Identifier", "http3.settings.id",
               FT_UINT64, BASE_HEX|BASE_VAL64_STRING, VALS64(http3_settings_vals), 0x0,
              NULL, HFILL }
        },
        { &hf_http3_settings_value,
            { "Settings Value", "http3.settings.value",
               FT_UINT64, BASE_DEC, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_settings_qpack_max_table_capacity,
            { "Max Table Capacity", "http3.settings.qpack.max_table_capacity",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_settings_max_field_section_size,
            { "Max header list size", "http3.settings.max_field_section_size",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "The default value is unlimited.", HFILL }
        },
        { &hf_http3_settings_qpack_blocked_streams,
            { "Blocked Streams", "http3.settings.qpack.blocked_streams",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_settings_extended_connect,
            { "Extended CONNECT", "http3.settings.extended_connect",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_settings_webtransport,
            { "WebTransport", "http3.settings.webtransport",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_settings_h3_datagram,
            { "H3 DATAGRAM", "http3.settings.h3_datagram",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_settings_h3_datagram_draft04,
            { "H3 DATAGRAM Draft04", "http3.settings.h3_datagram_draft04",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              NULL, HFILL }
        },

        /* Priority Update */
        { &hf_http3_priority_update_element_id,
            { "Priority Update Element ID", "http3.priority_update_element_id",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_http3_priority_update_field_value,
            { "Priority Update Field Value", "http3.priority_update_field_value",
              FT_STRING, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
    };

    static gint *ett[] = {&ett_http3,
                          &ett_http3_stream_uni,
                          &ett_http3_stream_bidi,
                          &ett_http3_frame,
                          &ett_http3_settings,
                          &ett_http3_headers,
                          &ett_http3_headers_qpack_blocked,
                          &ett_http3_qpack_update,
                          &ett_http3_qpack_opcode};

    static ei_register_info ei[] = {
        { &ei_http3_unknown_stream_type,
          { "http3.unknown_stream_type", PI_UNDECODED, PI_WARN,
            "An unknown stream type was encountered", EXPFILL }
        },
        //{ &ei_http3_data_not_decoded,
        //    { "http3.data_not_decoded", PI_UNDECODED, PI_WARN,
        //      "Data not decoded", EXPFILL }
        // },
        // { &ei_http3_qpack_enc_update,
        //   { "http3.qpack_enc_update", PI_UNDECODED, PI_WARN,
        //     "Success decoding QPACK buffer", EXPFILL }
        // },
         { &ei_http3_qpack_failed,
           { "http3.qpack_enc_failed", PI_UNDECODED, PI_NOTE,
             "Error decoding QPACK buffer", EXPFILL }
         },
         { &ei_http3_header_encoded_state ,
           { "http3.expert.header.encoded_state", PI_DEBUG, PI_NOTE,
             "HTTP3 header encoded block", EXPFILL }
         },
         { &ei_http3_header_decoding_failed ,
           { "http3.expert.header_decoding.failed", PI_UNDECODED, PI_NOTE,
             "Failed to decode HTTP3 header name/value", EXPFILL }
         },
         { &ei_http3_header_decoding_blocked,
           { "http3.expert.header_decoding.blocked", PI_UNDECODED, PI_NOTE,
             "Failed to decode HTTP3 header name/value (blocked on QPACK)", EXPFILL}
         },
         { &ei_http3_header_decoding_no_output,
           { "http3.expert.header_decoding.no_output", PI_UNDECODED, PI_NOTE,
             "Failed to decode HTTP3 header name/value (QPACK decoder no emission)", EXPFILL}
         },
    };

    proto_http3 = proto_register_protocol("Hypertext Transfer Protocol Version 3", "HTTP3", "http3");

    proto_register_field_array(proto_http3, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));

    module_http3 = prefs_register_protocol(proto_http3, NULL);

    expert_http3 = expert_register_protocol(proto_http3);
    expert_register_field_array(expert_http3, ei, array_length(ei));

    http3_handle = register_dissector("http3", dissect_http3, proto_http3);
#ifdef HAVE_NGHTTP3
    /* Fill hash table with static headers */
    register_static_headers();
#endif
}

void
proto_reg_handoff_http3(void)
{
    dissector_add_string("quic.proto", "h3", http3_handle);
}

/**
 * Implementation of helper functions.
 */
static http3_file_local_ctx *g_http3_file_local_ctx = NULL;

static guint
http3_conn_info_hash(gconstpointer key)
{
    guint8 bkey[QUIC_MAX_CID_LENGTH];
    const quic_cid_t *v;
    guint h = 0;

    if (key) {
        v = (const quic_cid_t *)key;
        memset(&bkey[0], 0, QUIC_MAX_CID_LENGTH);
        memcpy(&bkey[0], &v->cid[0], MIN(v->len, QUIC_MAX_CID_LENGTH));
        h = wmem_strong_hash(&bkey[0], QUIC_MAX_CID_LENGTH);
    }
    return h;
}

static gboolean
http3_conn_info_equal(gconstpointer lhs, gconstpointer rhs)
{
    const quic_cid_t *a    = (const quic_cid_t *)lhs;
    const quic_cid_t *b    = (const quic_cid_t *)rhs;
    size_t            alen = a->len;
    size_t            blen = b->len;

    return alen == blen && memcmp(&a->cid[0], &b->cid[0], alen) == 0;
}

#ifdef HAVE_NGHTTP3
/* Due to QPACK compression, we may get lots of relatively large
   header decoded_header_fields (e.g., 4KiB).  Allocating each of them requires lots
   of memory.  The maximum compression is achieved in QPACK by
   referencing header field stored in dynamic table by one or two
   bytes.  We reduce memory usage by caching header field in this
   wmem_map_t to reuse its memory region when we see the same header
   field next time. */

static size_t
http3_hdrcache_length(gconstpointer vv)
{
    const guint8 *v = (const guint8 *)vv;
    guint32       namelen, valuelen;

    namelen  = pntoh32(v);
    valuelen = pntoh32(v + sizeof(namelen) + namelen);

    return namelen + sizeof(namelen) + valuelen + sizeof(valuelen);
}

static guint
http3_hdrcache_hash(gconstpointer key)
{
    return wmem_strong_hash((const guint8 *)key, http3_hdrcache_length(key));
}

static gboolean
http3_hdrcache_equal(gconstpointer lhs, gconstpointer rhs)
{
    const guint8 *a    = (const guint8 *)lhs;
    const guint8 *b    = (const guint8 *)rhs;
    size_t        alen = http3_hdrcache_length(a);
    size_t        blen = http3_hdrcache_length(b);

    return alen == blen && memcmp(a, b, alen) == 0;
}

static size_t
http3_hdrdefcache_length(gconstpointer vv)
{
    const guint8 *v = (const guint8 *)vv;
    guint32       namelen;

    namelen = pntoh32(v);

    return namelen + sizeof(namelen);
}

static guint
http3_hdrdefcache_hash(gconstpointer key)
{
    return wmem_strong_hash((const guint8 *)key, http3_hdrdefcache_length(key));
}

static gboolean
http3_hdrdefcache_equal(gconstpointer lhs, gconstpointer rhs)
{
    const guint8 *a    = (const guint8 *)lhs;
    const guint8 *b    = (const guint8 *)rhs;
    size_t        alen = http3_hdrdefcache_length(a);
    size_t        blen = http3_hdrdefcache_length(b);

    return alen == blen && memcmp(a, b, alen) == 0;
}
#endif

/* Deallocation callback */
static bool
http3_file_local_ctx_del_cb(wmem_allocator_t *allocator _U_, wmem_cb_event_t event _U_, void *user_data _U_)
{
    g_http3_file_local_ctx = NULL;
    return FALSE;
}

static http3_file_local_ctx *
http3_get_file_local_ctx(void)
{
    if (g_http3_file_local_ctx == NULL) {
        /*
         * The file-local context hasn't been initialized yet
         * for the current file.
         */
        g_http3_file_local_ctx = wmem_new(wmem_file_scope(), http3_file_local_ctx);
        g_http3_file_local_ctx->conn_info_map =
            wmem_map_new(wmem_file_scope(), http3_conn_info_hash, http3_conn_info_equal);
#ifdef HAVE_NGHTTP3
        g_http3_file_local_ctx->hdr_cache_map =
            wmem_map_new(wmem_file_scope(), http3_hdrcache_hash, http3_hdrcache_equal);
        g_http3_file_local_ctx->hdr_def_cache_map =
            wmem_map_new(wmem_file_scope(), http3_hdrdefcache_hash, http3_hdrdefcache_equal);
#endif
        wmem_register_callback(wmem_file_scope(), http3_file_local_ctx_del_cb, NULL);
    }

    return g_http3_file_local_ctx;
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
