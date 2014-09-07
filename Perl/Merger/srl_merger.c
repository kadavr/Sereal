/* Must be defined before including Perl header files or we slow down by 2x! */
#define PERL_NO_GET_CONTEXT

#ifdef __cplusplus
extern "C" {
#endif
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"
#include "ppport.h"
#ifdef __cplusplus
}
#endif

#include <stdlib.h>

#ifndef PERL_VERSION
#    include <patchlevel.h>
#    if !(defined(PERL_VERSION) || (PERL_SUBVERSION > 0 && defined(PATCHLEVEL)))
#        include <could_not_find_Perl_patchlevel.h>
#    endif
#    define PERL_REVISION       5
#    define PERL_VERSION        PATCHLEVEL
#    define PERL_SUBVERSION     PERL_SUBVERSION
#endif
#if PERL_VERSION < 8
#   define PERL_MAGIC_qr                  'r' /* precompiled qr// regex */
#   define BFD_Svs_SMG_OR_RMG SVs_RMG
#elif ((PERL_VERSION==8) && (PERL_SUBVERSION >= 1) || (PERL_VERSION>8))
#   define BFD_Svs_SMG_OR_RMG SVs_SMG
#   define MY_PLACEHOLDER PL_sv_placeholder
#else
#   define BFD_Svs_SMG_OR_RMG SVs_RMG
#   define MY_PLACEHOLDER PL_sv_undef
#endif
#if (((PERL_VERSION == 9) && (PERL_SUBVERSION >= 4)) || (PERL_VERSION > 9))
#   define NEW_REGEX_ENGINE 1
#endif
#if (((PERL_VERSION == 8) && (PERL_SUBVERSION >= 1)) || (PERL_VERSION > 8))
#define MY_CAN_FIND_PLACEHOLDERS
#define HAS_SV2OBJ
#endif

#define SRL_HDR_SHORT_BINARY_LEN_FROM_TAG(tag) ((tag) & SRL_MASK_SHORT_BINARY_LEN)
#define SRL_HDR_ARRAYREF_LEN_FROM_TAG(tag) ((tag) & SRL_MASK_ARRAYREF_COUNT)
#define SRL_HDR_HASHREF_LEN_FROM_TAG(tag) ((tag) & SRL_MASK_HASHREF_COUNT)

#define SRL_GET_STRING_DEDUPER_AV(mrg)   ((mrg)->string_deduper_hv == NULL          \
                                         ? srl_init_string_deduper_hv(aTHX_ mrg)    \
                                         : (mrg)->string_deduper_hv)

#define SRL_GET_TRACKED_OFFSETS_TBL(mrg) ((mrg)->tracked_offsets_tbl == NULL        \
                                         ? srl_init_tracked_offsets_tbl(aTHX_ mrg)  \
                                         : (mrg)->tracked_offsets_tbl)

#define SRL_GET_TRACKED_OFFSETS(mrg)     ((mrg)->tracked_offsets == NULL            \
                                         ? srl_init_tracked_offsets(aTHX_ mrg)      \
                                         : (mrg)->tracked_offsets)

//#define SRL_MERGER_TRACE(msg, args...) warn((msg), args)
#define SRL_MERGER_TRACE(msg, args...)

#define SRL_REPORT_CURRENT_TAG(mrg, tag) STMT_START {                \
    SRL_MERGER_TRACE(                                                \
        "%s: tag %d (0x%X) at abs %d, rel %d (obuf abs %d, rel %d)", \
        __FUNCTION__, (tag), (tag),                                  \
        (int) BUF_POS_OFS(mrg->ibuf),                                \
        (int) BODY_POS_OFS(mrg->ibuf),                               \
        (int) BUF_POS_OFS(mrg->obuf),                                \
        (int) BODY_POS_OFS(mrg->obuf)                                \
    );                                                               \
} STMT_END

#define ASSERT_BUF_SPACE(buf, len, msg) STMT_START {         \
    if (expect_false((UV) BUF_SPACE((buf)) < (UV) (len))) {  \
        croak("Unexpected termination of packet%s, "         \
              "want %lu bytes, only have %lu available",     \
              (msg), (UV) (len), (UV) BUF_SPACE((buf)));     \
    }                                                        \
} STMT_END

#define GROW_BUF(buf, minlen) STMT_START {                            \
    DEBUG_ASSERT_BUF_SANE(buf);                                       \
    if (expect_false(BUF_NEED_GROW(buf, minlen))) {                   \
        srl_buf_grow_nocheck(aTHX_ &(buf), (BUF_SIZE(buf) + minlen)); \
        DEBUG_ASSERT_BUF_SANE(buf);                                   \
    }                                                                 \
} STMT_END

#define SRL_UPDATE_BUF_BODY_POS(buf, version) STMT_START { \
    if (expect_false((version) == 1)) {                    \
        SRL_SET_BODY_POS((buf), (buf).start);              \
    } else {                                               \
        SRL_SET_BODY_POS((buf), (buf).pos - 1);            \
    }                                                      \
} STMT_END

/* srl_buffer.h has set of functions (srl_buf_cat_*) which I need in merger,
 * but, for performance reason (avoid another level of inderection),
 * the functions want srl_encoder_t* as first parameter to access the buffer.
 * Hopefully, buffer is first field inside srl_merget_t structure, so if I
 * put buffer at same position it's semi-safely to cast srl_merget_t* to
 * srl_encoder_t*
 */
#define MRG2ENC(mrg) ((srl_encoder_t *) (mrg))
//#define SRL_MRG_HAVE_OPTION(mrg, flag_num) ((mrg)->flags & flag_num)
//#define SRL_MRG_SET_OPTION(mrg, flag_num) ((mrg)->flags |= flag_num)

#include "srl_merger.h"
#include "srl_common.h"
#include "ptable.h"
#include "srl_protocol.h"
#include "srl_inline.h"
#include "../Encoder/srl_buffer.h"

/* predeclare all our subs so we have one definitive authority for their signatures */
SRL_STATIC_INLINE UV srl_read_varint_uv_safe(pTHX_ srl_buffer_t *buf);
SRL_STATIC_INLINE UV srl_read_varint_uv_nocheck(pTHX_ srl_buffer_t *buf);
SRL_STATIC_INLINE UV srl_read_varint_uv(pTHX_ srl_buffer_t *buf);
SRL_STATIC_INLINE UV srl_read_varint_uv_offset(pTHX_ srl_buffer_t *buf, const char * const errstr);
SRL_STATIC_INLINE UV srl_read_varint_uv_length(pTHX_ srl_buffer_t *buf, const char * const errstr);
SRL_STATIC_INLINE UV srl_read_varint_uv_count(pTHX_ srl_buffer_t *buf, const char * const errstr);
SRL_STATIC_INLINE IV srl_validate_header_version_pv_len(pTHX_ char *strdata, STRLEN len);

SRL_STATIC_INLINE void srl_buf_copy_content(pTHX_ srl_merger_t *mrg, size_t len, const char * const errstr);
SRL_STATIC_INLINE void srl_buf_copy_content_nocheck(pTHX_ srl_merger_t *mrg, size_t len);
SRL_STATIC_INLINE void srl_buf_cat_tag_nocheck(pTHX_ srl_merger_t *mrg, const char tag);
SRL_STATIC_INLINE void srl_copy_varint(pTHX_ srl_merger_t *mrg);

SRL_STATIC_INLINE HV * srl_init_string_deduper_hv(pTHX_ srl_merger_t *mrg);
SRL_STATIC_INLINE ptable_ptr srl_init_tracked_offsets_tbl(pTHX_ srl_merger_t *mrg);
SRL_STATIC_INLINE srl_stack_t * srl_init_tracked_offsets(pTHX_ srl_merger_t *mrg);

SRL_STATIC_INLINE srl_merger_t * srl_empty_merger_struct(pTHX);                         /* allocate an empty merger struct - flags still to be set up */
SRL_STATIC_INLINE void srl_set_input_buffer(pTHX_ srl_merger_t *mrg, SV *src);        /* reset input buffer (ibuf) */
SRL_STATIC_INLINE void srl_build_track_table(pTHX_ srl_merger_t *mrg);
SRL_STATIC_INLINE void srl_merge_items(pTHX_ srl_merger_t *mrg);

SRL_STATIC_INLINE UV srl_expected_top_elements(pTHX_ srl_merger_t *mrg);
SRL_STATIC_INLINE void srl_store_tracked_offset(pTHX_ srl_merger_t *mrg, UV from, UV to);
SRL_STATIC_INLINE UV srl_lookup_tracked_offset(pTHX_ srl_merger_t *mrg, UV offset);
SRL_STATIC_INLINE UV srl_lookup_string(pTHX_ srl_merger_t *mrg, const char* src, STRLEN len, UV offset);

#define SvSIOK(sv) ((SvFLAGS(sv) & (SVf_IOK|SVf_IVisUV)) == SVf_IOK)
#define SvNSIV(sv) (SvNOK(sv) ? SvNVX(sv) : (SvSIOK(sv) ? SvIVX(sv) : sv_2nv(sv)))
SRL_STATIC_INLINE I32
S_sv_ncmp(pTHX_ SV *a, SV *b)
{
    NV nv1 = SvNSIV(a);
    NV nv2 = SvNSIV(b);
    return nv1 < nv2 ? -1 : nv1 > nv2 ? 1 : 0;
}

SRL_STATIC_INLINE ptable_ptr
srl_init_tracked_offsets_tbl(pTHX_ srl_merger_t *mrg)
{
    mrg->tracked_offsets_tbl = PTABLE_new();
    return mrg->tracked_offsets_tbl;
}

SRL_STATIC_INLINE HV *
srl_init_string_deduper_hv(pTHX_ srl_merger_t *mrg)
{
    mrg->string_deduper_hv = newHV();
    return mrg->string_deduper_hv;
}

SRL_STATIC_INLINE srl_stack_t *
srl_init_tracked_offsets(pTHX_ srl_merger_t *mrg)
{
    mrg->tracked_offsets = NULL;
    Newx(mrg->tracked_offsets, 1, srl_stack_t);

    if (expect_false(mrg->tracked_offsets == NULL))
        croak("Out of memory");

    if (expect_false(srl_stack_init(mrg->tracked_offsets, 16) != 0))
        croak("Out of memory");

    return mrg->tracked_offsets;
}

srl_merger_t *
srl_build_merger_struct(pTHX_ HV *opt)
{
    srl_merger_t *mrg;
    SV **svp;
    int i;

    mrg = srl_empty_merger_struct(aTHX);

    /* load options */
    if (opt != NULL) {
        /* Needs to be before the snappy options */
        /* mrg->protocol_version defaults to SRL_PROTOCOL_VERSION. */
        svp = hv_fetchs(opt, "protocol_version", 0);
        if (svp && SvOK(*svp)) {
            mrg->protocol_version = SvUV(*svp);
            if (mrg->protocol_version < 1 || mrg->protocol_version > SRL_PROTOCOL_VERSION) {
                croak("Specified Sereal protocol version ('%lu') is invalid",
                      (unsigned long)mrg->protocol_version);
            }
        }
    }

    /* 4 byte magic string + proto version
     * + potentially uncompressed size varint
     * +  1 byte varint that indicates zero-length header
     * +  1 byte SRL_HDR_REFN
     * +  1 byte SRL_HDR_ARRAY
     * +  SRL_MAX_VARINT_LENGTH bytes for padding varint */
    GROW_BUF(mrg->obuf, sizeof(SRL_MAGIC_STRING) + 1 + 1 + 1 + 1 + SRL_MAX_VARINT_LENGTH);

    if (expect_true(mrg->protocol_version > 2)) {
        srl_buf_cat_str_s_nocheck(MRG2ENC(mrg), SRL_MAGIC_STRING_HIGHBIT);
    } else {
        srl_buf_cat_str_s_nocheck(MRG2ENC(mrg), SRL_MAGIC_STRING);
    }

    srl_buf_cat_char_nocheck(MRG2ENC(mrg), (U8) mrg->protocol_version);
    srl_buf_cat_char_nocheck(MRG2ENC(mrg), '\0');

    SRL_UPDATE_BUF_BODY_POS(mrg->obuf, mrg->protocol_version);

    srl_buf_cat_char_nocheck(MRG2ENC(mrg), SRL_HDR_REFN);
    srl_buf_cat_char_nocheck(MRG2ENC(mrg), SRL_HDR_ARRAY);
    mrg->obuf_padding_bytes_offset = BUF_POS_OFS(mrg->obuf);

    for (i = 0; i < SRL_MAX_VARINT_LENGTH; ++i) {
        srl_buf_cat_char_nocheck(MRG2ENC(mrg), SRL_HDR_PAD);
    }

    return mrg;
}

void
srl_destroy_merger(pTHX_ srl_merger_t *mrg)
{
    srl_stack_destroy(aTHX_ &mrg->parser_stack);
    srl_buf_free_buffer(aTHX_ &mrg->obuf);

    if (mrg->tracked_offsets) {
        srl_stack_destroy(aTHX_ mrg->tracked_offsets);
        Safefree(mrg->tracked_offsets);
        mrg->tracked_offsets = NULL;
    }

    if (mrg->tracked_offsets_tbl) {
        PTABLE_free(mrg->tracked_offsets_tbl);
        mrg->tracked_offsets_tbl = NULL;
    }

    if (mrg->string_deduper_hv) {
        SvREFCNT_dec(mrg->string_deduper_hv);
        mrg->string_deduper_hv = NULL;
    }

    Safefree(mrg);
}

void
srl_merger_append(pTHX_ srl_merger_t *mrg, SV *src)
{
    assert(mrg != NULL);

    srl_set_input_buffer(mrg, src);
    srl_build_track_table(mrg);

    mrg->ibuf.pos = mrg->ibuf.body_pos + 1;
    srl_merge_items(mrg);
}

SV *
srl_merger_finish(pTHX_ srl_merger_t *mrg)
{
    assert(mrg != NULL);
    DEBUG_ASSERT_BUF_SANE(mrg->obuf);

    char* oldpos = mrg->obuf.pos;
    mrg->obuf.pos = mrg->obuf.start + mrg->obuf_padding_bytes_offset;
    DEBUG_ASSERT_BUF_SANE(mrg->obuf);

    srl_buf_cat_varint_nocheck(MRG2ENC(mrg), 0, mrg->cnt_of_merged_elements);
    DEBUG_ASSERT_BUF_SANE(mrg->obuf);

    mrg->obuf.pos = oldpos;
    DEBUG_ASSERT_BUF_SANE(mrg->obuf);

    return newSVpvn(mrg->obuf.start, BUF_POS_OFS(mrg->obuf));
}

SRL_STATIC_INLINE srl_merger_t *
srl_empty_merger_struct(pTHX)
{
    srl_merger_t *mrg = NULL;
    Newx(mrg, 1, srl_merger_t);
    if (mrg == NULL)
        croak("Out of memory");

    /* Init buffer struct */
    if (expect_false(srl_buf_init_buffer(aTHX_ &mrg->obuf, INITIALIZATION_SIZE) != 0)) {
        Safefree(mrg);
        croak("Out of memory");
    }

    /* Init parset stack struct */
    if (expect_false(srl_stack_init(aTHX_ &mrg->parser_stack, 32) != 0)) {
        srl_buf_free_buffer(aTHX_ &mrg->obuf);
        Safefree(mrg);
        croak("Out of memory");
    }

    /* Zero fields */
    mrg->cnt_of_merged_elements = 0;
    mrg->obuf_padding_bytes_offset = 0;
    mrg->protocol_version = SRL_PROTOCOL_VERSION;
    mrg->string_deduper_hv = NULL;
    mrg->tracked_offsets_tbl = NULL;
    mrg->tracked_offsets = NULL;
    return mrg;
}

SRL_STATIC_INLINE void
srl_set_input_buffer(pTHX_ srl_merger_t *mrg, SV *src)
{
    char *tmp;
    STRLEN len;
    UV header_len;
    U8 encoding_flags;
    U8 protocol_version;

    tmp = (char*) SvPV(src, len);
    mrg->ibuf.start = mrg->ibuf.pos = tmp;
    mrg->ibuf.end = mrg->ibuf.start + len;

    IV proto_version_and_encoding_flags_int = srl_validate_header_version_pv_len(aTHX_ mrg->ibuf.start, len);

    if (proto_version_and_encoding_flags_int < 1) {
        if (proto_version_and_encoding_flags_int == 0)
            croak("Bad Sereal header: It seems your document was accidentally UTF-8 encoded");
            //SRL_ERROR("Bad Sereal header: It seems your document was accidentally UTF-8 encoded");
        else
            croak("Bad Sereal header: Not a valid Sereal document.");
            //SRL_ERROR("Bad Sereal header: Not a valid Sereal document.");
    }

    mrg->ibuf.pos += 5;
    encoding_flags = (U8) (proto_version_and_encoding_flags_int & SRL_PROTOCOL_ENCODING_MASK);
    protocol_version = (U8) (proto_version_and_encoding_flags_int & SRL_PROTOCOL_VERSION_MASK);

    if (expect_false(protocol_version > 3 || protocol_version < 1)) {
        croak("Unsupported Sereal protocol version %u", protocol_version);
        //SRL_ERRORf1("Unsupported Sereal protocol version %u", dec->proto_version);
    }

    if (encoding_flags == SRL_PROTOCOL_ENCODING_RAW) {
        /* no op */
    } else if (   encoding_flags == SRL_PROTOCOL_ENCODING_SNAPPY
               || encoding_flags == SRL_PROTOCOL_ENCODING_SNAPPY_INCREMENTAL)
    {
        croak("snappy compression is not implemented");
    } else if (encoding_flags == SRL_PROTOCOL_ENCODING_ZLIB) {
        croak("zlib compression is not implemented");
    } else {
        croak("Sereal document encoded in an unknown format '%d'",
              encoding_flags >> SRL_PROTOCOL_VERSION_BITS);
//        SRL_ERRORf1("Sereal document encoded in an unknown format '%d'",
//                    dec->encoding_flags >> SRL_PROTOCOL_VERSION_BITS);
    }

    // skip header in any case
    header_len = srl_read_varint_uv_length(&mrg->ibuf, " while reading header");

    if (protocol_version > 1 && header_len) {
        mrg->ibuf.pos += header_len - 1;
    } else {
        mrg->ibuf.pos += header_len;
    }

    SRL_UPDATE_BUF_BODY_POS(mrg->ibuf, protocol_version);
    DEBUG_ASSERT_BUF_SANE(mrg->ibuf);
}

SRL_STATIC_INLINE void
srl_build_track_table(pTHX_ srl_merger_t *mrg)
{
    U8 tag;
    UV offset, length;

    DEBUG_ASSERT_BUF_SANE(mrg->ibuf);

    if (mrg->tracked_offsets)
        srl_stack_clear(mrg->tracked_offsets);

    while (BUF_NOT_DONE(mrg->ibuf)) {
        tag = *mrg->ibuf.pos;
        if (expect_false(tag & SRL_HDR_TRACK_FLAG)) {
            tag = tag & ~SRL_HDR_TRACK_FLAG;
            offset = BODY_POS_OFS(mrg->ibuf);
            srl_stack_push(SRL_GET_TRACKED_OFFSETS(mrg), offset);
        }

        SRL_REPORT_CURRENT_TAG(mrg, tag);
        mrg->ibuf.pos++;

        if (tag >= SRL_HDR_SHORT_BINARY_LOW) {
            mrg->ibuf.pos += (int) SRL_HDR_SHORT_BINARY_LEN_FROM_TAG(tag);
        } else if (tag > SRL_HDR_NEG_HIGH && tag < SRL_HDR_ARRAYREF_LOW) {
            switch (tag) {
                case SRL_HDR_VARINT:
                case SRL_HDR_ZIGZAG:
                    srl_read_varint_uv(&mrg->ibuf); // TODO test/implement srl_skip_varint()
                    break;

                case SRL_HDR_FLOAT:         mrg->ibuf.pos += 4;     break;
                case SRL_HDR_DOUBLE:        mrg->ibuf.pos += 8;     break;
                case SRL_HDR_LONG_DOUBLE:   mrg->ibuf.pos += 16;    break;

                case SRL_HDR_BINARY:
                case SRL_HDR_STR_UTF8:
                    length = srl_read_varint_uv_length(&mrg->ibuf, " while reading BINARY or STR_UTF8");
                    mrg->ibuf.pos += length;
                    break;

                case SRL_HDR_HASH:
                case SRL_HDR_ARRAY:
                    srl_read_varint_uv_count(&mrg->ibuf, " while reading ARRAY or HASH");
                    break;

                case SRL_HDR_TRUE:
                case SRL_HDR_FALSE:
                case SRL_HDR_UNDEF:
                case SRL_HDR_CANONICAL_UNDEF:
                    // noop
                    break;

                default:
                    switch (tag) {
                        case SRL_HDR_COPY:
                        case SRL_HDR_ALIAS:
                        case SRL_HDR_REFP:
                        case SRL_HDR_OBJECTV:
                        case SRL_HDR_OBJECTV_FREEZE:
                            offset = srl_read_varint_uv_offset(&mrg->ibuf, " while reading COPY/ALIAS/REFP/OBJECTV/OBJECTV_FREEZE");
                            srl_stack_push(SRL_GET_TRACKED_OFFSETS(mrg), offset);
                            break;

                        case SRL_HDR_PAD:
                        case SRL_HDR_REFN:
                        case SRL_HDR_WEAKEN:
                        case SRL_HDR_EXTEND:
                        case SRL_HDR_REGEXP:
                        case SRL_HDR_OBJECT:
                        case SRL_HDR_OBJECT_FREEZE:
                            // noop
                            break;

                        default:
                            croak("unexpected"); // TODO SRL_ERROR_UNEXPECTED(dec,tag, " single value");
                            break;
                    }
            }
        }
    }

    if (mrg->tracked_offsets && !srl_stack_empty(mrg->tracked_offsets)) {
        srl_stack_rsort(mrg->tracked_offsets);
        srl_stack_dedupe(mrg->tracked_offsets);

        //int i = 0;
        //int64_t *ptr = mrg->tracked_offsets->begin;
        //while (ptr <= mrg->tracked_offsets->ptr) {
        //    warn("tracked_offsets: offset dedups idx %d offset %d\n", i, (int) *ptr);
        //    i++; ptr++;
        //}
    }

    DEBUG_ASSERT_BUF_SANE(mrg->ibuf);
}

SRL_STATIC_INLINE void
srl_merge_items(pTHX_ srl_merger_t *mrg)
{
    U8 tag;
    UV length, offset;
    UV itag_offset, otag_offset;
    UV expected_top_elements = srl_expected_top_elements(mrg);
    srl_stack_t *tracked_offsets = mrg->tracked_offsets;
    srl_stack_t *parser_stack = &mrg->parser_stack;
    int64_t *stack_ptr;
    bool trackme;

    DEBUG_ASSERT_BUF_SANE(mrg->ibuf);
    DEBUG_ASSERT_BUF_SANE(mrg->obuf);

    // parser_stack is needed for two things:
    // - keep track of expected things
    // - verify document consistency
    srl_stack_clear(parser_stack);
    srl_stack_push(parser_stack, expected_top_elements);

    while (BUF_NOT_DONE(mrg->ibuf) && !srl_stack_empty(parser_stack)) {
        DEBUG_ASSERT_BUF_SANE(mrg->ibuf);
        DEBUG_ASSERT_BUF_SANE(mrg->obuf);

        tag = *mrg->ibuf.pos;
        tag = tag & ~SRL_HDR_TRACK_FLAG;
        SRL_REPORT_CURRENT_TAG(mrg, tag);

        itag_offset = BODY_POS_OFS(mrg->ibuf);
        otag_offset = BODY_POS_OFS(mrg->obuf);
        stack_ptr = srl_stack_ptr(parser_stack);

        trackme = tracked_offsets
                  && !srl_stack_empty(tracked_offsets)
                  && (int) itag_offset == srl_stack_peek_nocheck(tracked_offsets);

        if (tag <= SRL_HDR_NEG_HIGH) {
            srl_buf_cat_tag_nocheck(mrg, tag);
        } else if (tag >= SRL_HDR_ARRAYREF_LOW && tag <= SRL_HDR_ARRAYREF_HIGH) {
            srl_buf_cat_tag_nocheck(mrg, tag);
            srl_stack_push(parser_stack, SRL_HDR_ARRAYREF_LEN_FROM_TAG(tag));
        } else if (tag >= SRL_HDR_HASHREF_LOW && tag <= SRL_HDR_HASHREF_HIGH) {
            srl_buf_cat_tag_nocheck(mrg, tag);
            srl_stack_push(parser_stack, SRL_HDR_HASHREF_LEN_FROM_TAG(tag) * 2);
        } else if (tag >= SRL_HDR_SHORT_BINARY_LOW) {
            length = SRL_HDR_SHORT_BINARY_LEN_FROM_TAG(tag);

            // +1 because need to respect tag
            ASSERT_BUF_SPACE(mrg->ibuf, length + 1, " while reading SHORT_BINARY");
            offset = srl_lookup_string(mrg, mrg->ibuf.pos + 1, length, otag_offset);

            if (offset) {
                // issue COPY tag
                srl_buf_cat_varint(MRG2ENC(mrg), SRL_HDR_COPY, offset);
                mrg->ibuf.pos += length + 1;
                otag_offset = offset;
            } else {
                // see string first time
                srl_buf_copy_content_nocheck(mrg, length + 1);
            }
        } else {
            switch (tag) {
                case SRL_HDR_VARINT:
                case SRL_HDR_ZIGZAG:
                    srl_buf_cat_tag_nocheck(mrg, tag);
                    srl_copy_varint(mrg);
                    break;

                case SRL_HDR_FLOAT:         srl_buf_copy_content(mrg, 5,  " while copying FLOAT");       break;
                case SRL_HDR_DOUBLE:        srl_buf_copy_content(mrg, 9,  " while copying DOUBLE");      break;
                case SRL_HDR_LONG_DOUBLE:   srl_buf_copy_content(mrg, 17, " while copying LONG_DOUBLE"); break;

                case SRL_HDR_TRUE:
                case SRL_HDR_FALSE:
                case SRL_HDR_UNDEF:
                case SRL_HDR_CANONICAL_UNDEF:
                    srl_buf_cat_tag_nocheck(mrg, tag);
                    break;

                case SRL_HDR_BINARY:
                case SRL_HDR_STR_UTF8:
                    mrg->ibuf.pos++; // skip tag in input buffer
                    length = srl_read_varint_uv_length(&mrg->ibuf, " while reading BINARY or STR_UTF8");
                    offset = srl_lookup_string(mrg, mrg->ibuf.pos, length, otag_offset);

                    if (offset) {
                        // issue COPY tag
                        srl_buf_cat_varint(MRG2ENC(mrg), SRL_HDR_COPY, offset);
                        mrg->ibuf.pos += (int) length;
                        otag_offset = offset;
                    } else {
                        // see string first time
                        srl_buf_cat_varint(MRG2ENC(mrg), tag, length);
                        srl_buf_copy_content_nocheck(mrg, length); // srl_read_varint_uv_length() do space assertion
                    }

                    break;

                case SRL_HDR_HASH:
                case SRL_HDR_ARRAY:
                    mrg->ibuf.pos++; // skip tag in input buffer
                    length = srl_read_varint_uv_count(&mrg->ibuf, " while reading ARRAY or HASH");
                    srl_buf_cat_varint((srl_encoder_t*) mrg, tag, length);
                    srl_stack_push(parser_stack, tag == SRL_HDR_HASH ? length * 2 : length);
                    break;

                default:
                    switch (tag) {
                        case SRL_HDR_COPY:
                        case SRL_HDR_REFP:
                            mrg->ibuf.pos++; // skip tag in input buffer
                            offset = srl_read_varint_uv_offset(&mrg->ibuf, " while reading COPY/ALIAS/REFP/OBJECTV/OBJECTV_FREEZE");
                            offset = srl_lookup_tracked_offset(mrg, offset); // convert ibuf offset to obuf offset
                            srl_buf_cat_varint((srl_encoder_t*) mrg, tag, offset);

                            if (tag == SRL_HDR_REFP) {
                                SRL_SET_TRACK_FLAG(*(mrg->obuf.body_pos + offset));
                            }

                            break;

                        case SRL_HDR_PAD:
                        case SRL_HDR_REFN:
                        case SRL_HDR_WEAKEN:
                        case SRL_HDR_EXTEND:
                            // this set of tags are not real items,
                            // and stack_item_value should not be decremented,
                            // but at the end of the loop I dont want to create if-branch
                            // so, I simply increment counter to level furter decremetion
                            srl_stack_incr_value(parser_stack, stack_ptr, 1);
                            srl_buf_cat_tag_nocheck(mrg, tag);
                            break;

                        //case SRL_HDR_ALIAS:
                        //case SRL_HDR_OBJECTV:
                        //case SRL_HDR_OBJECTV_FREEZE:
                        //    offset = srl_read_varint_uv_offset(&mrg->ibuf, " while reading COPY/ALIAS/REFP/OBJECTV/OBJECTV_FREEZE");
                        //    av_push(mrg->tracked_offsets_with_duplicates, newSVuv(offset));
                        //    break;
                        //
                        //case SRL_HDR_REGEXP:
                        //case SRL_HDR_OBJECT:
                        //case SRL_HDR_OBJECT_FREEZE:
                        //    // noop
                        //    break;

                         default:
                            croak("unexpected tag %d (0x%X) at %d", tag, tag, (int) BUF_POS_OFS(mrg->ibuf));
                            break;
                    }
            }
        }

        srl_stack_incr_value(parser_stack, stack_ptr, -1);

        while (expect_false(
              !srl_stack_empty(parser_stack)
            && srl_stack_peek_nocheck(parser_stack) == 0
        )) {
            srl_stack_pop(parser_stack);
        }

        if (expect_false(trackme)) {
            srl_stack_pop(tracked_offsets);
            srl_store_tracked_offset(mrg, itag_offset, otag_offset);
        }
    }

    DEBUG_ASSERT_BUF_SANE(mrg->ibuf);
    DEBUG_ASSERT_BUF_SANE(mrg->obuf);

    if (expect_false(!srl_stack_empty(parser_stack)))
        croak("parser_stack is not empty, unexpected termination"); // TODO

    mrg->cnt_of_merged_elements += expected_top_elements;
}

SRL_STATIC_INLINE UV
srl_expected_top_elements(pTHX_ srl_merger_t *mrg)
{
    return 1;
}

SRL_STATIC_INLINE void
srl_store_tracked_offset(pTHX_ srl_merger_t *mrg, UV from, UV to)
{
    SRL_MERGER_TRACE("srl_store_tracked_offset: %d -> %d", (int) from, (int) to);
    PTABLE_store(SRL_GET_TRACKED_OFFSETS_TBL(mrg), (void *) from, (void *) to);
}

SRL_STATIC_INLINE UV
srl_lookup_tracked_offset(pTHX_ srl_merger_t *mrg, UV offset)
{
    void *res = PTABLE_fetch(SRL_GET_TRACKED_OFFSETS_TBL(mrg), (void *) offset);
    if (expect_false(!res))
        croak("bad target offset %d", offset); // TODO

    UV len = (UV) res;
    SRL_MERGER_TRACE("srl_lookup_tracked_offset: %d -> %d", (int) offset, (int) len);
    if (expect_false(mrg->obuf.body_pos + len >= mrg->obuf.pos)) {
        croak("srl_lookup_tracked_offset: corrupted packet");
    }

    return len;
}

SRL_STATIC_INLINE UV
srl_lookup_string(pTHX_ srl_merger_t *mrg, const char* src, STRLEN len, UV offset)
{
    if (len <= 3)
        return (UV) 0; // 0 is invalid offset for all Sereal versions

    SV **offset_ptr = hv_fetch(SRL_GET_STRING_DEDUPER_AV(mrg), src, len, 1);
    if (expect_false(offset_ptr == NULL)) {
        croak("out of memory (hv_fetch returned NULL)");
    }

    if (SvOK(*offset_ptr)) {
        UV target = SvUV(*offset_ptr);
        SRL_MERGER_TRACE("srl_lookup_string: got duplicate for '%.*s' target %d",
                         (int) len, src, (int) target);

        return target;
    }

    SRL_MERGER_TRACE("srl_lookup_string: not found duplicate for '%.*s'",
                     (int) len, src);

    sv_setuv(*offset_ptr, offset);
    return (UV) 0; // 0 is invalid offset for all Sereal versions
}

SRL_STATIC_INLINE void
srl_buf_copy_content(pTHX_ srl_merger_t *mrg, size_t len, const char * const errstr)
{
    DEBUG_ASSERT_BUF_SANE(mrg->ibuf);
    ASSERT_BUF_SPACE(mrg->ibuf, len, errstr);
    srl_buf_copy_content_nocheck(mrg, len);
}

SRL_STATIC_INLINE void
srl_buf_copy_content_nocheck(pTHX_ srl_merger_t *mrg, size_t len)
{
    GROW_BUF(mrg->obuf, len);

    // TODO profile for len == 1
    Copy(mrg->ibuf.pos, mrg->obuf.pos, len, char);
    mrg->ibuf.pos += len;
    mrg->obuf.pos += len;

    DEBUG_ASSERT_BUF_SANE(mrg->ibuf);
    DEBUG_ASSERT_BUF_SANE(mrg->obuf);
}

SRL_STATIC_INLINE void
srl_buf_cat_tag_nocheck(pTHX_ srl_merger_t *mrg, const char tag)
{
    GROW_BUF(mrg->obuf, 1);

    *mrg->obuf.pos++ = tag;
    mrg->ibuf.pos++;

    DEBUG_ASSERT_BUF_SANE(mrg->ibuf);
    DEBUG_ASSERT_BUF_SANE(mrg->obuf);
}

SRL_STATIC_INLINE void
srl_copy_varint(pTHX_ srl_merger_t *mrg)
{
    unsigned int lshift = 0;

    DEBUG_ASSERT_BUF_SANE(mrg->ibuf);
    GROW_BUF(mrg->obuf, SRL_MAX_VARINT_LENGTH);

    while (BUF_NOT_DONE(mrg->ibuf) && *mrg->ibuf.pos & 0x80) {
        *mrg->obuf.pos++ = *mrg->ibuf.pos++;
        lshift += 7;

        if (expect_false(lshift > (sizeof(UV) * 8)))
            croak("varint too big");
            //SRL_ERROR("varint too big");
    }

    if (expect_true(BUF_NOT_DONE(mrg->ibuf))) {
        *mrg->obuf.pos++ = *mrg->ibuf.pos++;
    } else {
        croak("varint terminated prematurely");
        //SRL_ERROR("varint terminated prematurely");
    }

    DEBUG_ASSERT_BUF_SANE(mrg->ibuf);
    DEBUG_ASSERT_BUF_SANE(mrg->obuf);
}

/* VARINT function
 * copy-pasted from srl_decoder.h
 */

SRL_STATIC_INLINE UV
srl_read_varint_uv(pTHX_ srl_buffer_t *buf)
{
    if (expect_true(BUF_SPACE(*buf) > 10))
        return srl_read_varint_uv_nocheck(aTHX_ buf);
    else
        return srl_read_varint_uv_safe(aTHX_ buf);
}

SRL_STATIC_INLINE UV
srl_read_varint_uv_safe(pTHX_ srl_buffer_t *buf)
{
    UV uv= 0;
    unsigned int lshift= 0;

    while (BUF_NOT_DONE(*buf) && *buf->pos & 0x80) {
        uv |= ((UV) (*buf->pos++ & 0x7F) << lshift);
        lshift += 7;
        if (lshift > (sizeof(UV) * 8))
            croak("varint too big");
            //SRL_ERROR("varint too big");
    }

    if (expect_true(BUF_NOT_DONE(*buf))) {
        uv |= ((UV) *buf->pos++ << lshift);
    } else {
        croak("varint terminated prematurely");
        //SRL_ERROR("varint terminated prematurely");
    }

    return uv;
}

SRL_STATIC_INLINE UV
srl_read_varint_uv_nocheck(pTHX_ srl_buffer_t *buf)
{
    UV uv= 0;
    unsigned int lshift= 0;

    while (*buf->pos & 0x80) {
        uv |= ((UV) (*buf->pos++ & 0x7F) << lshift);
        lshift += 7;

        if (expect_false(lshift > (sizeof(UV) * 8)))
            croak("varint too big");
            //SRL_ERROR("varint too big");
    }

    uv |= ((UV) (*buf->pos++) << lshift);
    return uv;
}

SRL_STATIC_INLINE UV
srl_read_varint_uv_offset(pTHX_ srl_buffer_t *buf, const char * const errstr)
{
    UV len= srl_read_varint_uv(aTHX_ buf);
    if (expect_false(buf->body_pos + len >= buf->pos)) {
        croak("Corrupted packet%s. Offset %lu points past current iposition %lu in packet with length of %lu bytes long",
              errstr, (unsigned long)len, (unsigned long) BUF_POS_OFS(*buf), (unsigned long) BUF_SIZE(*buf));
    }

    return len;
}

SRL_STATIC_INLINE UV
srl_read_varint_uv_length(pTHX_ srl_buffer_t *buf, const char * const errstr)
{
    UV len= srl_read_varint_uv(aTHX_ buf);
    ASSERT_BUF_SPACE(*buf, len, errstr);
    return len;
}

SRL_STATIC_INLINE UV
srl_read_varint_uv_count(pTHX_ srl_buffer_t *buf, const char * const errstr)
{
    UV len= srl_read_varint_uv(aTHX_ buf);
    if (len > I32_MAX) {
        croak("Corrupted packet%s. Count %lu exceeds I32_MAX (%i), which is imipossible.", errstr, len, I32_MAX);
        //SRL_ERRORf3("Corrupted packet%s. Count %lu exceeds I32_MAX (%i), which is imipossible.", errstr, len, I32_MAX);
    }

    return len;
}

SRL_STATIC_INLINE IV
srl_validate_header_version_pv_len(pTHX_ char *strdata, STRLEN len)
{
    if (len >= SRL_MAGIC_STRLEN + 3) {
        /* + 3 above because:
         * at least one version/flag byte,
         * one byte for header len,
         * one type byte (smallest payload)
         */

        /* Do NOT do *((U32*)strdata at least for these reasons:
         * (1) Unaligned access can "Bus error" on you
         *     (char* can be much less aligned than U32).
         * (2) In ILP64 even if aligned the U32 would be 64 bits wide,
         *     and the deref would read 8 bytes, more than the smallest
         *     (valid) message.
         * (3) Endianness.
         */
        U8 version_encoding= strdata[SRL_MAGIC_STRLEN];
        U8 version= version_encoding & SRL_PROTOCOL_VERSION_MASK;

        if (memEQ(SRL_MAGIC_STRING, strdata, SRL_MAGIC_STRLEN)) {
            if ( 0 < version && version < 3 ) {
                return version_encoding;
            }
        } else if (memEQ(SRL_MAGIC_STRING_HIGHBIT, strdata, SRL_MAGIC_STRLEN)) {
            if ( 3 <= version ) {
                return version_encoding;
           }
        } else if (memEQ(SRL_MAGIC_STRING_HIGHBIT_UTF8, strdata, SRL_MAGIC_STRLEN)) {
            return 0;
        }
    }

    return -1;
}
