//#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__MINGW32__) || !defined(_WIN32)
#include <unistd.h>
#endif
#include "tb_sha256.h"
#include "tb_aes.h"
#include "tb_md5.h"
#include "tb_crc32.h"
#include "../deps/zlib/zutil.h"

#define ZXCFG_VER	"1.2"

#define DEF_KEY		"PON_Dkey"
#define DEF_IV		"PON_DIV"
#define USER_KEY	"8cc72b05705d5c46f412af8cbed55aad"
#define USER_IV		"667b02a85c61c786def4521b060265e8"

#define CFG_HDR_CFGTYPE		2
#define CFG_HDR_DEFCFGTYPE	0
#define CFG_HDR_DEF_MODEL	"ZXHN F7015TV3"
#define CFG_HDR_MAGIC		"\x99\x99\x99\x99\x44\x44\x44\x44\x55\x55\x55\x55\xAA\xAA\xAA\xAA"
#define CFG_MODEL_MAGIC		0x04030201
#define XML_HDR_MAGIC		0x01020304	// Big-Endian
#define SPLIT_BLK_SIZE		0x10000
#define MAX_PARAMTAG_FSIZE	10240		// max paramtag file size
#define PARAMTAGHDR			"TAGH"		// paramtag file header

// /etc/hardcode
#define HC_KEY	"09a01cee5518b341f40d83f1cc5e7c2ac3631ee2fd87c3b85b6b586194cc5486F70xx_5p4"

#define GET_U32(p) (((p)[0] << 24) | ((p)[1] << 16) | ((p)[2] << 8) | ((p)[3]))
#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif

typedef struct
{
	tb_uint32 magic[4];				// \x99\x99\x99\x99\x44\x44\x44\x44\x55\x55\x55\x55\xAA\xAA\xAA\xAA
	tb_uint32 offset;				// offset + 20, usually 0
} cfg_hdr_magic_t;

typedef struct 
{
	tb_uint32 rsv1;					// 0
	tb_uint32 x4;					// 4
	tb_uint32 rsv2[8];				// 0
	tb_uint32 offset;				// usually 0x40
} cfg_hdr_4_t;

typedef struct 
{
	tb_uint16 icfgtype;				// 2
	tb_uint16 idefcfgtype;			// 0
	tb_uint32 x80;					// 0x80
	tb_uint32 file_size;			//
	tb_uint32 rsv3[13];				// 0
} cfg_hdr_type_t;

typedef struct 
{
	tb_uint32 magic;				// \x04\x03\x02\x01
	tb_uint32 rsv1;
	tb_uint32 name_len;
} cfg_hdr_model_t;

typedef struct
{
	cfg_hdr_magic_t hdr_magic;
	cfg_hdr_4_t hdr_4;
	cfg_hdr_type_t hdr_type;
	cfg_hdr_model_t hdr_model;
} cfg_hdr_t;

typedef struct 
{
	tb_uint32 magic;				// \x01\x02\x03\x04
	tb_uint32 ver;					// 0 or 1 or 2 or 3 or 4
	tb_uint32 rsv[13];				// 0
} encrypt_hdr_t;

typedef struct 
{
	tb_uint32 plain_len;			//
	tb_uint32 cipher_len;			//
	tb_uint32 have_next_blk;		// 
} encrypt_blk_hdr_t;

typedef struct 
{
	tb_uint32 magic;				// \x01\x02\x03\x04
	tb_uint32 ver;					// 0
	tb_uint32 content_len;			// uncompressed file content length
	tb_uint32 last_compr_blk_off;	// last compression block offset
	tb_uint32 split_blk_size;		// 0x10000
	tb_uint32 content_crc;
	tb_uint32 hdr_crc;				// crc([magic~content_crc])
	tb_uint32 rsv[8];
} compr_file_hdr_t;

typedef struct 
{
	tb_uint32 blk_size;				// original block size
	tb_uint32 compred_blk_size;		// compressed block size
	tb_uint32 next_compred_blk_off;	// next compressed block offset
} compr_blk_hdr_t;

typedef struct
{
	const char* file_in;
	const char* file_out;
	const char* file_paramtag;
	const char* key_arg;			// hardcode key
	FILE* fp_in;
	char key[36];
	char iv[36];
	tb_uint32 aes_key_ext[60];
	tb_uint8 aes_iv[16];
	tb_uint16 cfgtype;
	tb_uint16 defcfgtype;
	tb_uint8 model_name[260];		// model_name[0] is len
	tb_uint8 mode;
	tb_uint8 pack_type;
	tb_uint8 key_method;
	tb_bool bo_le;					// is byte-order little-endian
	tb_bool iv_input;				// specify iv manually
} zxcfg_t;

typedef struct
{
	char keys[2][36];
	tb_int32 count;
	tb_int32 idx;
} keyset_t;

void usage(const char* app)
{
	fprintf(stderr, "Usage: %s [OPTIONS]\n"
		"Options:\n"
		"  -i  input file name\n"
		"  -o  output file name\n"
		"  -m  mode\n"
		"      0 --- unpack cfg or xml file (default mode)\n"
		"      1 --- pack into xml file\n"
		"      2 --- pack into cfg file\n"
		"      3 --- unpack hardcode file(/etc/hardcodefile/)\n"
		"  -t  pack type (pack mode only)\n"
		"      0 --- compress\n"
		"      1 --- compress, encrypt with default key\n"
		"      2 --- compress, encrypt with user key\n"
		"  -k  aescbc encrypt & decrypt key\n"
		"  -v  aescbc encrypt & decrypt iv\n"
		"  -g  generate aescbc key method\n"
		"      0 --- sha256(default, if the \"-k\" option is not specified)\n"
		"      1 --- md5, sha256(default, if the \"-k\" option is specified)\n"
		"  -n  device model name.(only used to pack into cfg. default: " 
		       CFG_HDR_DEF_MODEL ")\n"
		"  -l  byte order.(only used to pack into cfg. default: 0)\n"
		"      0 --- big endian\n"
		"      1 --- little endian\n"
		"  -c  cfg type (only used to pack into cfg. default: 2)\n"
		"  -d  defcfg type (only used to pack into cfg. default: 0)\n"
		"  -p  specify the paramtag file(Used only to unpack files)\n"
		"  -h  show this help message\n"
		"\n", app);
	fprintf(stderr, "Unpacking example:\n");
	fprintf(stderr, "  %s -i ctce8_F663N.cfg -o f663n.txt\n", app);
	fprintf(stderr, "  %s -i ctce8_ZXHN_F650(GPON_ONU).cfg -o f650.txt\n", app);
	fprintf(stderr, "  %s -i ctce8_F7610M.cfg -o f7610m.txt -k D245264C8493b272360e\n", app);
	fprintf(stderr, "  %s -i g7615.xml -o g7615.txt -p paramtag_g7615\n", app);
	fprintf(stderr, "  %s -i db_default_cfg.xml -o default.txt\n", app);
	fprintf(stderr, "\nPacking example:\n");
	fprintf(stderr, "  %s -i f663n.txt -o f663n.cfg -m 2 -t 0 -n F663N\n", app);
	fprintf(stderr, "  %s -i f650.txt -o f650.cfg -m 2 -t 2 -n \"ZXHN F650(GPON ONU)\" -l 1\n", app);
	fprintf(stderr, "  %s -i f7610m.txt -o f7610m.cfg -m 2 -t 2 -n F7610M -l 1 -k D245264C8493b272360e\n", app);
	fprintf(stderr, "  %s -i g7615.txt -o g7615_pack.xml -m 1 -t 2\n", app);
	fprintf(stderr, "  %s -i default.txt -o default_pack.xml -m 1 -t 1\n", app);
}

// is little endian
static inline tb_bool is_le(void)
{
	const tb_uint16 n = 0x0001;
	return *(tb_uint8*)&n;
}

tb_uint16 h2ns(tb_uint16 n)
{
	if(is_le())
		n = (n >> 8) | (n << 8);

	return n;
}

tb_uint32 h2nl(tb_uint32 n)
{
	if(is_le())
		n = (n >> 24) | (n << 24) | ((n & 0xFF0000) >> 8) | 
			((n & 0xFF00) << 8);

	return n;
}

int proc_args(zxcfg_t* zc, int argc, char* argv[])
{
	int i, ret = -1;
	tb_bool m = tb_false;

	if(argc <= 1 || strcmp(argv[1], "-h") == 0)
	{
		usage(argv[0]);
		goto lbl_exit;
	}

	for(i=1; i<argc; i++)
	{
		if(argv[i][0] != '-' || i + 1 >= argc)
			continue;

		switch(argv[i][1])
		{
		case 'i':
			zc->file_in = argv[++i];
			break;

		case 'o':
			zc->file_out = argv[++i];
			break;

		case 'm':
			zc->mode = (tb_uint8)atoi(argv[++i]);
			if(zc->mode > 3)
			{
				printf("Invalid mode: %u !!!\n", zc->mode);
				goto lbl_exit;
			}
			break;

		case 't':
			zc->pack_type = (tb_uint8)atoi(argv[++i]);
			if(zc->pack_type > 2)
			{
				printf("Invalid pack type: %u !!!\n", zc->pack_type);
				goto lbl_exit;
			}
			break;

		case 'k':
			sprintf(zc->key, "%.32s", argv[++i]);
			zc->key_arg = argv[i];
			break;

		case 'v':
			sprintf(zc->iv, "%.32s", argv[++i]);
			zc->iv_input = tb_true;
			break;

		case 'g':
			zc->key_method = (tb_uint8)atoi(argv[++i]);
			if(zc->key_method > 1)
			{
				printf("Invalid key method: %u !!!\n", zc->key_method);
				goto lbl_exit;
			}
			m = tb_true;
			break;

		case 'n':
			zc->model_name[0] = (tb_uint8)sprintf((char*)&zc->model_name[1], 
				"%.255s", argv[++i]);
			break;

		case 'l':
			zc->bo_le = '0' != argv[++i][0];
			break;

		case 'c':
			zc->cfgtype = (tb_uint16)atoi(argv[++i]);
			break;

		case 'd':
			zc->defcfgtype = (tb_uint16)atoi(argv[++i]);
			break;

		case 'p':
			zc->file_paramtag = argv[++i];
			break;
		}
	}

	if(NULL == zc->file_in || NULL == zc->file_out)
	{
		fprintf(stderr, "The input and output file must be specified!\n");
		goto lbl_exit;
	}

	zc->fp_in = fopen(zc->file_in, "rb");
	if(NULL == zc->fp_in)
	{
		fprintf(stderr, "Can not open file: %s\n", zc->file_in);
		goto lbl_exit;
	}

	if(zc->key_arg != NULL && !m)
		zc->key_method = 1;

	if(2 == zc->mode && 0 == zc->model_name[0])
	{
		zc->model_name[0] = sizeof(CFG_HDR_DEF_MODEL) - 1;
		memcpy(&zc->model_name[1], CFG_HDR_DEF_MODEL, zc->model_name[0]);
	}

	ret = 0;

lbl_exit:
	return ret;
}

tb_int32 bin2hex(tb_uint8* bin, tb_uint32 len, char* hex, tb_bool add_end_char)
{
	const char* tab = "0123456789abcdef";
	tb_uint32 i;

	for(i=0; i<len; i++)
	{
		hex[i * 2] = tab[bin[i] >> 4];
		hex[i * 2 + 1] = tab[bin[i] & 0xF];
	}

	if(add_end_char)
		hex[i * 2] = '\0';

	return i * 2;
}

tb_uint32 get_file_size(FILE* fp)
{
	tb_uint32 off, ret;

	off = ftell(fp);
	fseek(fp, 0, SEEK_END);
	ret = ftell(fp);
	fseek(fp, off, SEEK_SET);

	return ret;
}

void gen_key_iv(zxcfg_t* zc, tb_uint32 ver)
{
	tb_uint8 key[32], iv[32];
	tb_int32 l;

	if(NULL == zc->key_arg)
		sprintf(zc->key, "%.32s", 4 == ver ? USER_KEY : DEF_KEY);

	if(!zc->iv_input)
		sprintf(zc->iv, "%.32s", 4 == ver ? USER_IV : DEF_IV);

	if(1 != zc->key_method)
	{
		l = (tb_int32)strlen(zc->key);
		tb_sha256(zc->key, min(l, 31), key);
	}
	else
	{
		tb_uint8 m5[16];
		char chs[32];

		tb_md5(zc->key, 33, m5);
		bin2hex(m5, 16, chs, 0);
		tb_sha256(chs, 31, key);
	}

	tb_aes_key_setup(key, zc->aes_key_ext, 256);
	l = (tb_int32)strlen(zc->iv);
	tb_sha256(zc->iv, min(l, 31), iv);
	memcpy(zc->aes_iv, iv, 16);
}

void update_key(zxcfg_t* zc, const char* newkey)
{
	tb_uint8 key[32], m5[16];
	char chs[32];

	tb_md5(newkey, 33, m5);
	bin2hex(m5, 16, chs, 0);
	tb_sha256(chs, 31, key);
	tb_aes_key_setup(key, zc->aes_key_ext, 256);
}

tb_int32 decrypt_xml(zxcfg_t* zc, FILE* fpr, const char* tmpfile, FILE** fpout)
{
	FILE* fpw = NULL;
	encrypt_blk_hdr_t ebh;
	tb_uint8 *pi = NULL, *po = NULL;
	tb_uint32 rl, blk_idx = 0;
	tb_int32 r, ret = -1;

	fpw = fopen(tmpfile, "wb+");
	if(NULL == fpw)
	{
		fprintf(stderr, "Can not create temp file: %s\n", tmpfile);
		return -1;
	}

	pi = (tb_uint8 *)malloc(SPLIT_BLK_SIZE);
	po = (tb_uint8 *)malloc(SPLIT_BLK_SIZE);

	if(NULL == pi || NULL == po)
	{
		fprintf(stderr, "Alloc memory failed!\n");
		goto lbl_exit;
	}

	do
	{
		rl = (tb_uint32)fread(&ebh, 1, sizeof(ebh), fpr);
		if(rl != (tb_uint32)sizeof(ebh))
		{
			fprintf(stderr, "Read encrypted block header error!\n");
			goto lbl_exit;
		}

		ebh.plain_len = h2nl(ebh.plain_len);
		ebh.cipher_len = h2nl(ebh.cipher_len);
		ebh.have_next_blk = h2nl(ebh.have_next_blk);

		if(ebh.plain_len > ebh.cipher_len || ebh.cipher_len > SPLIT_BLK_SIZE ||
			ebh.cipher_len % TB_AES_BLOCK_SIZE != 0)
		{
			fprintf(stderr, "Cipher len error!\n");
			goto lbl_exit;
		}

		
		rl = (tb_uint32)fread(pi, 1, ebh.cipher_len, fpr);
		if(rl != ebh.cipher_len)
		{
			fprintf(stderr, "Read encrypted block data error!\n");
			goto lbl_exit;
		}

		r = tb_aes_decrypt_cbc(pi, ebh.cipher_len, po, zc->aes_key_ext, 256, 
			zc->aes_iv);

		if(0 == r)
		{
			if(blk_idx++ == 0)
			{
				if(GET_U32(po) == XML_HDR_MAGIC && GET_U32(po + 4) == 0)
					fwrite(po, ebh.plain_len, 1, fpw);
				else
					goto lbl_err_msg;
			}
			else
				fwrite(po, ebh.plain_len, 1, fpw);
		}
		else
		{
lbl_err_msg:
			ret = -2; // decrypt error
			fprintf(stderr, "Decrypt error!\n");
			goto lbl_exit;
		}
	} while(ebh.have_next_blk != 0);

	ret = 0;

lbl_exit:
	free(pi);
	free(po);

	if(ret != 0)
	{
		fclose(fpw);
		unlink(tmpfile);
	}
	else
	{
		fseek(fpw, 0, SEEK_SET);
		*fpout = fpw;
		printf("Decrypt ok!\n");
	}

	return ret;
}

tb_int32 uncompress_data(const void* in, tb_uint32 in_len, 
	void* out, tb_uint32* out_len)
{
	z_stream zs;
	tb_int32 ret = -1;

	zs.next_in = (Bytef*)in;
	zs.avail_in = in_len;
	zs.next_out = (Bytef*)out;
	zs.avail_out = *out_len;
	zs.zalloc = NULL;
	zs.zfree = NULL;
	if(inflateInit(&zs) == Z_OK)
	{
		int err = inflate(&zs, Z_FINISH);
		if(Z_STREAM_END == err)
		{
			*out_len = zs.total_out;
			inflateEnd(&zs);
			ret = 0;
		}
		else
		{
			fprintf(stderr, "inflate ret: %d, msg: %s\n", err, zs.msg);
			inflateEnd(&zs);
		}
	}

	return ret;
}

tb_int32 compress_data(const void* in, tb_uint32 in_len, 
	void* out, tb_uint32* out_len)
{
	z_stream zs;
	tb_int32 ret = -1;

	zs.next_in = (Bytef*)in;
	zs.avail_in = in_len;
	zs.next_out = (Bytef*)out;
	zs.avail_out = *out_len;
	zs.zalloc = NULL;
	zs.zfree = NULL;
	if(deflateInit(&zs, MAX_MEM_LEVEL) == Z_OK)
	{
		int err = deflate(&zs, Z_FINISH);
		if(Z_STREAM_END == err)
		{
			*out_len = zs.total_out;
			deflateEnd(&zs);
			ret = 0;
		}
		else
			deflateEnd(&zs);
	}

	return ret;
}

tb_int32 uncompress_xml(zxcfg_t* zc, FILE* fpr, compr_file_hdr_t* cfh)
{
	compr_blk_hdr_t cbh;
	tb_crc32_t crc;
	tb_uint32 rl, hdr_crc, out_len;
	FILE* fpw = NULL;
	tb_uint8 *pi = NULL, *po = NULL;
	tb_int32 ret = -1;

	tb_crc32_init(&crc);
	hdr_crc = h2nl(tb_crc32(cfh, (tb_uint32)((tb_uint8*)&cfh->hdr_crc - 
		(tb_uint8*)cfh)));

	if(h2nl(cfh->split_blk_size) != SPLIT_BLK_SIZE || hdr_crc != cfh->hdr_crc)
	{
		fprintf(stderr, "Invalid compressed file header!\n");
		return -1;
	}

	pi = (tb_uint8*)malloc(SPLIT_BLK_SIZE);
	po = (tb_uint8*)malloc(SPLIT_BLK_SIZE);

	if(NULL == pi || NULL == po)
	{
		fprintf(stderr, "Alloc memory error!\n");
		goto lbl_exit;
	}

	fpw = fopen(zc->file_out, "wb");
	if(NULL == fpw)
	{
		fprintf(stderr, "Can not open file: %s\n", zc->file_out);
		goto lbl_exit;
	}

	do
	{
		rl = (tb_uint32)fread(&cbh, 1, sizeof(cbh), fpr);
		if(rl != (tb_uint32)sizeof(cbh))
		{
			fprintf(stderr, "Read compressed block header error! rl:%u\n", rl);
			goto lbl_exit;
		}

		cbh.blk_size = h2nl(cbh.blk_size);
		cbh.compred_blk_size = h2nl(cbh.compred_blk_size);
		cbh.next_compred_blk_off = h2nl(cbh.next_compred_blk_off);

		if(cbh.compred_blk_size > SPLIT_BLK_SIZE)
		{
			fprintf(stderr, "Compressed block header data error!\n");
			goto lbl_exit;
		}

		if(fread(pi, 1, cbh.compred_blk_size, fpr) != cbh.compred_blk_size)
		{
			fprintf(stderr, "Read compressed block data error!\n");
			goto lbl_exit;
		}

		tb_crc32_update(&crc, pi, cbh.compred_blk_size);
		out_len = SPLIT_BLK_SIZE;
		ret = uncompress_data(pi, cbh.compred_blk_size, po, &out_len);
		if(ret != 0)
		{
			fprintf(stderr, "Uncompress data error!\n");
			goto lbl_exit;
		}

		fwrite(po, out_len, 1, fpw);
		
	} while(cbh.next_compred_blk_off != 0);

	if(h2nl(tb_crc32_final(&crc)) != cfh->content_crc)
	{
		fprintf(stderr, "Check compressed data crc failed!\n");
		goto lbl_exit;
	}

	printf("Uncompress ok!\n");
	ret = 0;

lbl_exit:
	if(fpw != NULL)
		fclose(fpw);

	if(ret != 0)
		unlink(zc->file_out);

	free(pi);
	free(po);
	return ret;
}

tb_int32 cp_content(zxcfg_t* zc, FILE* fpr, const char* tmpfile, FILE** fpout)
{
	FILE* fpw;
	compr_blk_hdr_t bh;
	tb_uint8 *pi = NULL;
	tb_uint32 rl;
	tb_int32 ret = -1;
	(void)zc; // unused

	fpw = fopen(tmpfile, "wb+");
	if(NULL == fpw)
	{
		fprintf(stderr, "Can not create file: %s\n", tmpfile);
		return -1;
	}

	pi = (tb_uint8*)malloc(SPLIT_BLK_SIZE);
	if(NULL == pi)
	{
		fprintf(stderr, "Alloc memory failed! len: %u\n", SPLIT_BLK_SIZE);
		goto lbl_exit;
	}

	do
	{
		rl = (tb_uint32)fread(&bh, 1, sizeof(bh), fpr);
		if(rl != (tb_uint32)sizeof(bh))
		{
			fprintf(stderr, "Read file error! rl:%u\n", rl);
			goto lbl_exit;
		}

		bh.compred_blk_size = h2nl(bh.compred_blk_size);

		if(bh.compred_blk_size - 1 >= SPLIT_BLK_SIZE)
		{
			fprintf(stderr, "Invalid split block size!\n");
			goto lbl_exit;
		}

		rl = (tb_uint32)fread(pi, 1, bh.compred_blk_size, fpr);
		if(rl != bh.compred_blk_size)
		{
			fprintf(stderr, "Read block data error!\n");
			goto lbl_exit;
		}

		fwrite(pi, bh.compred_blk_size, 1, fpw);
	} while(bh.next_compred_blk_off != 0);

	ret = 0;

lbl_exit:
	free(pi);

	if(ret != 0)
	{
		fclose(fpw);
		unlink(tmpfile);
	}
	else
	{
		fseek(fpw, 0, SEEK_SET);
		*fpout = fpw;
	}

	return ret;
}

static tb_int32 get_key_from_paramtag(zxcfg_t* zc, keyset_t* ks)
{
#define ID_INDIVKEY	1824
#define ID_GPONSN	2177
#define ID_PONMAC	32769
#define ID_MAC1		256

	typedef struct
	{
		tb_uint16 id;
		tb_uint16 len;
		tb_uint8* val;
	} tag_t;

	FILE* fp = NULL;
	tb_uint8 *mem = NULL, *p, *endp;
	tag_t indivkey = {ID_INDIVKEY, 0, NULL}, gponsn = {ID_GPONSN, 0, NULL};
	tag_t ponmac = {ID_PONMAC, 0, NULL}, mac1 = {ID_MAC1, 0, NULL};
	tag_t *pts[] = {&indivkey, &gponsn, &ponmac, &mac1};
	char mac1_str[16] = "00d0d0000001";	// 00:d0:d0:00:00:01
	char ponmac_str[16];
	tb_uint32 fsize;
	tb_uint16 id, len;
	tb_uint32 i;
	tb_int32 ret = -1;
	tb_bool le;

	fp = fopen(zc->file_paramtag, "rb");
	if(NULL == fp)
	{
		printf("Can not open file: %s\n", zc->file_paramtag);
		goto lbl_exit;
	}

	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if(fsize > MAX_PARAMTAG_FSIZE)
	{
		fprintf(stderr, "The paramtag file is too large!\n");
		goto lbl_exit;
	}

	mem = (tb_uint8 *)malloc(fsize);
	if(NULL == mem)
	{
		fprintf(stderr, "Alloc memory failed! size: %u\n", fsize);
		goto lbl_exit;
	}

	fread(mem, fsize, 1, fp);

	if(memcmp(mem, PARAMTAGHDR, sizeof(PARAMTAGHDR) - 1) != 0)
	{
		printf("Invalid paramtag file header!\n");
		goto lbl_exit;
	}

	p = mem + 20;
	endp = mem + fsize;

	if(p + 8 > endp || p[4] == p[5] || (p[4] != 0 && p[5] != 0))
	{
		printf("Invalid paramtag file!\n");
		goto lbl_exit;
	}

	le = p[4] > 0;
	do
	{
		if(le)
		{
			id = (p[1] << 8) | p[0];
			len = (p[5] << 8) | p[4];
		}
		else
		{
			id = (p[0] << 8) | p[1];
			len = (p[4] << 8) | p[5];
		}

		for(i=0; i<sizeof(pts)/sizeof(pts[0]); i++)
		{
			if(pts[i]->id == id && NULL == pts[i]->val)
			{
				pts[i]->val = p + 6;
				pts[i]->len = len;
			}
		}

		p += (9U + len) & ~3U;
	} while(p + 8 <= endp);

	memset(zc->key, 0, sizeof(zc->key));
	if(NULL == indivkey.val)
	{
		tb_int32 idx = 0;

		if(mac1.val != NULL)
			bin2hex(mac1.val, 6, mac1_str, tb_false);

		sprintf(zc->key, "00000001%s", mac1_str);

		if(gponsn.val != NULL)
		{
			len = (tb_uint16)min(gponsn.len, 20);
			memset(ks->keys[idx], 0, sizeof(ks->keys[idx]));
			memcpy(ks->keys[idx], gponsn.val, len);
			memcpy(&ks->keys[idx++][len], mac1_str, 13);
		}

		if(ponmac.val != NULL)
		{
			bin2hex(ponmac.val, 6, ponmac_str, tb_false);
			memset(ks->keys[idx], 0, sizeof(ks->keys[idx]));
			memcpy(ks->keys[idx], ponmac_str, 12);
			memcpy(&ks->keys[idx++][12], mac1_str, 13);
		}

		ks->count = idx;
	}
	else
		memcpy(zc->key, indivkey.val, min(indivkey.len, 32));

	zc->key_method = 1;
	zc->key_arg = zc->key;
	ret = 0;

lbl_exit:
	if(fp != NULL)
		fclose(fp);

	free(mem);
	return ret;
}

tb_int32 unpack_xml(zxcfg_t* zc)
{
	union{
		encrypt_hdr_t eh;
		compr_file_hdr_t cfh;
	} xh;
	
	char tmpname[FILENAME_MAX];
	FILE* fptmp = NULL;
	keyset_t ks;
	tb_uint32 ver, fpos = 0;
	tb_int32 ret = -1;

	if(fread(&xh, 1, sizeof(xh), zc->fp_in) != sizeof(xh))
	{
		fprintf(stderr, "Read xml header error!\n");
		goto lbl_exit;
	}

	if(h2nl(xh.cfh.magic) != XML_HDR_MAGIC)
	{
		fprintf(stderr, "Invalid xml header magic!\n");
		goto lbl_exit;
	}

	ver = h2nl(xh.eh.ver);
	if(ver > 4)
	{
		fprintf(stderr, "Invalid file version code: %u!\n", ver);
		goto lbl_exit;
	}

	tmpname[FILENAME_MAX - 1] = '\0';
	printf("Xml db ver: %u\n", ver);
	switch(ver)
	{
	case 0: // uncompress
		ret = uncompress_xml(zc, zc->fp_in, &xh.cfh);
		break;

	case 1: // copy content
		ret = cp_content(zc, zc->fp_in, zc->file_out, &fptmp);
		if(0 == ret)
			fclose(fptmp);
		break;

	case 2: // copy content & uncompress
		snprintf(tmpname, FILENAME_MAX - 1, "%s.unc", zc->file_out);
		ret = cp_content(zc, zc->fp_in, tmpname, &fptmp);
		if(0 == ret)
		{
			if(fread(&xh, 1, sizeof(xh), fptmp) != sizeof(xh))
			{
				fprintf(stderr, "Read temp file xml header error!\n");
				ret = -1;
			}
			else
				ret = uncompress_xml(zc, fptmp, &xh.cfh);
			fclose(fptmp);
			unlink(tmpname);
		}
		break;

	//case 3: // decrypt with default key & uncompress
	//case 4: // decrypt with user key & uncompress
	default:
		snprintf(tmpname, FILENAME_MAX - 1, "%s.dec", zc->file_out);
		ks.count = 0;
		ks.idx = 0;
		if(4 == ver && zc->file_paramtag != NULL)
		{
			ret = get_key_from_paramtag(zc, &ks);
			if(ret != 0)
				goto lbl_exit;
		}

		gen_key_iv(zc, ver);
		printf("Use key: %s\n", zc->key);
		printf("Use iv: %s\n", zc->iv);
		printf("Generate key method: %s\n", 
			zc->key_method ? "md5, sha256" : "sha256");
		fpos = ftell(zc->fp_in);

lbl_retry:
		ret = decrypt_xml(zc, zc->fp_in, tmpname, &fptmp);
		if(0 == ret)
		{
			if(fread(&xh, 1, sizeof(xh), fptmp) != sizeof(xh))
			{
				fprintf(stderr, "Read temp file xml header error!\n");
				ret = -1;
			}
			else
				ret = uncompress_xml(zc, fptmp, &xh.cfh);
			fclose(fptmp);
			unlink(tmpname);
		}
		else if(-2 == ret) // decrypt error
		{
			if(ks.idx < ks.count)
			{
				printf("Use key: %s\n", ks.keys[ks.idx]);
				update_key(zc, ks.keys[ks.idx]);
				ks.idx++;
				fseek(zc->fp_in, fpos, SEEK_SET);
				goto lbl_retry;
			}
			else
				fprintf(stderr, "Please use the \"-k\" option and "
					"try a different key.\n");
		}
		break;
	}

	if(0 == ret)
		printf("Unpack ok!\n");

lbl_exit:
	return ret;
}

tb_int32 unpack(zxcfg_t* zc)
{
	cfg_hdr_magic_t chm;
	char buf[256], *p;
	tb_int32 ret = -1;
	
	p = buf;
	if(fread(&chm, 1, sizeof(chm), zc->fp_in) != sizeof(chm))
	{
		fprintf(stderr, "Read cfg magic header error!\n");
		goto lbl_exit;
	}

	if(memcmp(chm.magic, CFG_HDR_MAGIC, sizeof(CFG_HDR_MAGIC) - 1) == 0)
	{
		cfg_hdr_4_t h4;
		cfg_hdr_type_t ht;
		cfg_hdr_model_t hm;
		tb_uint32 u32;

		if(chm.offset != 0)
		{
			u32 = h2nl(chm.offset);
			if(u32 > 0xFFFF)
				fseek(zc->fp_in, chm.offset, SEEK_CUR);
			else
				fseek(zc->fp_in, u32, SEEK_CUR);
		}

		if(fread(&h4, 1, sizeof(h4), zc->fp_in) != sizeof(h4))
		{
			fprintf(stderr, "Read cfg header error!\n");
			goto lbl_exit;
		}

		u32 = h2nl(h4.x4);
		if(0x04 == u32)
		{
			zc->bo_le = tb_false;
			u32 = is_le() ? h2nl(h4.offset) : h4.offset;
		}
		else if(0x04000000 == u32)
		{
			zc->bo_le = tb_true;
			u32 = is_le() ?  h4.offset : h2nl(h4.offset);
		}
		else
		{
			fprintf(stderr, "Cfg header 4 data error!\n");
			goto lbl_exit;
		}

		fseek(zc->fp_in, u32, SEEK_SET);
		if(fread(&ht, 1, sizeof(ht), zc->fp_in) != sizeof(ht))
		{
			fprintf(stderr, "Read cfg type header error!\n");
			goto lbl_exit;
		}

		if(is_le() != zc->bo_le)
		{
			ht.icfgtype = h2ns(ht.icfgtype);
			ht.idefcfgtype = h2ns(ht.idefcfgtype);
			ht.x80 = h2nl(ht.x80);
			ht.file_size = h2nl(ht.file_size);
		}

		fseek(zc->fp_in, 0, SEEK_END);
		if((tb_uint32)ftell(zc->fp_in) - 128 != ht.file_size)
		{
			fprintf(stderr, "Cfg header type data error!\n");
			goto lbl_exit;
		}

		fseek(zc->fp_in, 128, SEEK_SET);
		if(fread(&hm, 1, sizeof(hm), zc->fp_in) != sizeof(hm))
		{
			fprintf(stderr, "Read cfg model header error!\n");
			goto lbl_exit;
		}

		u32 = h2nl(hm.name_len);
		if(u32 > sizeof(buf) - 1)
		{
			p = (char*)malloc(u32 + 1);
			if(NULL == p)
			{
				fprintf(stderr, "Alloc name memory failed! len:%u\n", u32);
				goto lbl_exit;
			}
		}
		if(fread(p, 1, u32, zc->fp_in) != u32)
		{
			fprintf(stderr, "Read model name error!\n");
			goto lbl_exit;
		}
		p[u32] = '\0';
		printf("Cfg header info:\n");
		printf("  Byte-order: %u(%s)\n", zc->bo_le, 
			zc->bo_le ? "Little-Endian" : "Big-Endian");

		printf("  icfgtype: %u\n  idefcfgtype: %u\n",
			ht.icfgtype, ht.idefcfgtype);
		printf("  Model name: %s\n", p);
	}
	else
		fseek(zc->fp_in, 0, SEEK_SET);

	ret = unpack_xml(zc);

lbl_exit:
	if(p != buf)
		free(p);

	return ret;
}

#define LAST_COMPR_BLK_OFF

tb_int32 compress_xml(zxcfg_t* zc, const char* tmpfile, FILE** fpout, 
	tb_uint32 cur_off)
{
	FILE* fpw = NULL;
	compr_file_hdr_t cfh;
	compr_blk_hdr_t* cbh;
	tb_crc32_t crc;
	tb_uint32 content_len = 0, rl, ol, next_compred_blk_off, blk_len;
	tb_uint8 *pi = NULL, *po = NULL;
	tb_int32 ret = -1;

	pi = (tb_uint8 *)malloc(SPLIT_BLK_SIZE);
	po = (tb_uint8 *)malloc(sizeof(compr_blk_hdr_t) + SPLIT_BLK_SIZE);

	if(NULL == pi || NULL == po)
	{
		fprintf(stderr, "Alloc memory failed!\n");
		goto lbl_exit;
	}

	cbh = (compr_blk_hdr_t*)po;
	if(tmpfile != NULL)
	{
		fpw = fopen(tmpfile, "wb+");
		if(NULL == fpw)
		{
			fprintf(stderr, "Can not open file: %s\n", tmpfile);
			goto lbl_exit;
		}
	}
	else fpw = *fpout;

	content_len = get_file_size(zc->fp_in);
	tb_crc32_init(&crc);
	memset(&cfh, 0, sizeof(cfh));
	cfh.magic = h2nl(XML_HDR_MAGIC);
	cfh.ver = 0;
	cfh.content_len = h2nl(content_len);
	cfh.split_blk_size = h2nl(SPLIT_BLK_SIZE);

	// skip file header
	fseek(fpw, sizeof(cfh), SEEK_CUR);
	next_compred_blk_off = (tb_uint32)sizeof(cfh);

	while(content_len > 0)
	{
		rl = content_len > SPLIT_BLK_SIZE ? SPLIT_BLK_SIZE : content_len;
		content_len -= rl;

		ol = (tb_uint32)fread(pi, 1, rl, zc->fp_in);
		if(ol != rl)
		{
			fprintf(stderr, "Read data error!\n");
			goto lbl_exit;
		}

		cbh->blk_size = h2nl(rl);
		ol = SPLIT_BLK_SIZE;
		if(compress_data(pi, rl, po + sizeof(compr_blk_hdr_t), &ol) != 0)
		{
			fprintf(stderr, "Compress data failed!\n");
			goto lbl_exit;
		}

		cbh->compred_blk_size = h2nl(ol);
		blk_len = (tb_uint32)sizeof(compr_blk_hdr_t) + ol;
		tb_crc32_update(&crc, po + sizeof(compr_blk_hdr_t), ol);

#ifndef LAST_COMPR_BLK_OFF
		cfh.last_compr_blk_off = next_compred_blk_off + blk_len;
#endif
		if(content_len != 0)
		{
			next_compred_blk_off += blk_len;
#ifdef LAST_COMPR_BLK_OFF
			cfh.last_compr_blk_off = next_compred_blk_off;
#endif
		}
		else
			next_compred_blk_off = 0;

		cbh->next_compred_blk_off = h2nl(next_compred_blk_off);
		fwrite(po, blk_len, 1, fpw);
	}

	cfh.last_compr_blk_off = h2nl(cfh.last_compr_blk_off);
	cfh.content_crc = h2nl(tb_crc32_final(&crc));
	cfh.hdr_crc = h2nl(tb_crc32(&cfh, (tb_uint32)((tb_uint8*)&cfh.hdr_crc - 
		(tb_uint8*)&cfh.magic)));
	fseek(fpw, cur_off, SEEK_SET);
	fwrite(&cfh, sizeof(cfh), 1, fpw);
	ret = 0;

lbl_exit:
	if(ret != 0)
	{
		if(fpw != NULL)
			fclose(fpw);

		if(tmpfile != NULL)
			unlink(tmpfile);
	}
	else
	{
		*fpout = fpw;
		fseek(fpw, 0, SEEK_SET);
		printf("Compress ok!\n");
	}

	free(pi);
	free(po);
	return ret;
}

tb_int32 encrypt_xml(zxcfg_t* zc, FILE* fpr, FILE* fpw, tb_uint32 dbver)
{
	encrypt_hdr_t eh;
	encrypt_blk_hdr_t* ebh;
	tb_uint32 content_len, rl, ol;
	tb_uint8 *pi = NULL, *po = NULL;
	tb_int32 ret = -1;

	memset(&eh, 0, sizeof(eh));
	eh.magic = h2nl(XML_HDR_MAGIC);
	eh.ver = h2nl(dbver);

	fwrite(&eh, sizeof(eh), 1, fpw);
	content_len = get_file_size(fpr);

	pi = (tb_uint8 *)malloc(SPLIT_BLK_SIZE);
	po = (tb_uint8 *)malloc(sizeof(encrypt_blk_hdr_t) + SPLIT_BLK_SIZE);

	if(NULL == pi || NULL == po)
	{
		fprintf(stderr, "Alloc memory failed!\n");
		goto lbl_exit;
	}

	ebh = (encrypt_blk_hdr_t*)po;
	while(content_len > 0)
	{
		rl = content_len > SPLIT_BLK_SIZE ? SPLIT_BLK_SIZE : content_len;
		content_len -= rl;

		ol = (tb_uint32)fread(pi, 1, rl, fpr);
		if(ol != rl)
		{
			fprintf(stderr, "Read data error!\n");
			goto lbl_exit;
		}
		
		ebh->plain_len = h2nl(rl);
		if(rl % TB_AES_BLOCK_SIZE != 0)
		{
			ol = (tb_uint32)TB_ALIGN(rl, TB_AES_BLOCK_SIZE);
			while(rl < ol)
				pi[rl++] = 0;
		}

		ebh->cipher_len = h2nl(rl);
		ebh->have_next_blk = h2nl(content_len != 0 ? 1 : 0);
		tb_aes_encrypt_cbc(pi, rl, po + sizeof(encrypt_blk_hdr_t), 
			zc->aes_key_ext, 256, zc->aes_iv);
		
		fwrite(po, sizeof(encrypt_blk_hdr_t) + rl, 1, fpw);
	}

	printf("Encrypt ok!\n");
	ret = 0;

lbl_exit:
	free(pi);
	free(po);

	return ret;
}

tb_int32 pack(zxcfg_t* zc, tb_bool is_cfg)
{
	const char* packtype[3] = {"compress", 
		"compress, encrypt with default key", 
		"compress, encrypt with user key"};
	const tb_uint32 dbvers[3] = {0, 3, 4};
	cfg_hdr_t ch;
	tb_uint32 dbver;
	FILE* fp_out;
	tb_int32 ret = -1;

	fp_out = fopen(zc->file_out, "wb");
	if(NULL == fp_out)
	{
		fprintf(stderr, "Can not open file: %s\n", zc->file_out);
		return -1;
	}

	printf("Pack %s\n", is_cfg ? "cfg" : "xml");
	if(is_cfg)
	{
		if(0 == zc->model_name[0])
		{
			zc->model_name[0] = sizeof(CFG_HDR_DEF_MODEL) - 1;
			memcpy(&zc->model_name[1], CFG_HDR_DEF_MODEL, zc->model_name[0]);
		}

		memset(&ch, 0, sizeof(ch));
		memcpy(ch.hdr_magic.magic, CFG_HDR_MAGIC, sizeof(CFG_HDR_MAGIC) - 1);
		ch.hdr_4.x4 = 4;
		ch.hdr_4.offset = (tb_uint32)(sizeof(cfg_hdr_magic_t) + 
			sizeof(cfg_hdr_4_t));
		ch.hdr_type.icfgtype = zc->cfgtype;
		ch.hdr_type.idefcfgtype = zc->defcfgtype;
		ch.hdr_type.x80 = 0x80;
		//ch.hdr_type.file_size = 0;
		ch.hdr_model.magic = CFG_MODEL_MAGIC;
		ch.hdr_model.name_len = zc->model_name[0];
		
		if(is_le() != zc->bo_le)
		{
			ch.hdr_4.x4 = h2nl(ch.hdr_4.x4);
			ch.hdr_4.offset = h2nl(ch.hdr_4.offset);
			ch.hdr_type.icfgtype = h2ns(ch.hdr_type.icfgtype);
			ch.hdr_type.idefcfgtype = h2ns(ch.hdr_type.idefcfgtype);
			ch.hdr_type.x80 = h2nl(ch.hdr_type.x80);
		}

		if(is_le())
		{
			ch.hdr_model.magic = h2nl(ch.hdr_model.magic);
			ch.hdr_model.name_len = h2nl(ch.hdr_model.name_len);
		}

		printf("Cfg header:\n");
		printf("  Byte-order: %u(%s)\n", zc->bo_le, 
			zc->bo_le ? "Little-Endian" : "Big-Endian");
		printf("  icfgtype: %u\n  idefcfgtype: %u\n", 
			zc->cfgtype, zc->defcfgtype);
		printf("  Model name: %s\n", &zc->model_name[1]);

		fwrite(&ch, sizeof(ch), 1, fp_out);
		fwrite(&zc->model_name[1], zc->model_name[0], 1, fp_out);
	}

	dbver = dbvers[zc->pack_type];
	printf("Pack type: %s\n", packtype[zc->pack_type]);
	printf("Xml db ver: %u\n", dbver);
	if(zc->pack_type != 0) // compress & encrypt
	{
		FILE* fptmp = NULL;
		char tmpfile[FILENAME_MAX];

		gen_key_iv(zc, dbver);
		printf("Use key: %s\n", zc->key);
		printf("Use iv: %s\n", zc->iv);
		printf("Generate key method: %s\n", 
			zc->key_method ? "md5, sha256" : "sha256");

		tmpfile[FILENAME_MAX - 1] = '\0';
		snprintf(tmpfile, FILENAME_MAX - 1, "%s.cpr", zc->file_out);
		ret = compress_xml(zc, tmpfile, &fptmp, 0);
		if(0 == ret)
		{
			ret = encrypt_xml(zc, fptmp, fp_out, dbver);
			fclose(fptmp);
			unlink(tmpfile);
			if(ret != 0)
			{
				fclose(fp_out);
				unlink(zc->file_out);
			}
		}
		else
		{
			fclose(fp_out);
			unlink(zc->file_out);
		}
	}
	else
	{
		ret = compress_xml(zc, NULL, &fp_out, ftell(fp_out));
		if(0 != ret)
			unlink(zc->file_out);
	}

	if(0 == ret)
	{
		if(is_cfg)
		{
			fseek(fp_out, 0, SEEK_END);
			ch.hdr_type.file_size = ftell(fp_out) - 
				sizeof(cfg_hdr_magic_t) - sizeof(cfg_hdr_4_t) - 
				sizeof(cfg_hdr_type_t);

			if(is_le() != zc->bo_le)
				ch.hdr_type.file_size = h2nl(ch.hdr_type.file_size);

			fseek(fp_out, (tb_uint32)((tb_uint8*)&ch.hdr_type.file_size - 
				(tb_uint8*)&ch), SEEK_SET);
			fwrite(&ch.hdr_type.file_size, sizeof(ch.hdr_type.file_size),
				1, fp_out);
		}
		fclose(fp_out);
	}
	
	return ret;
}

tb_int32 decrypt_hc(zxcfg_t* zc)
{
	tb_uint8 *pi = NULL, *po = NULL;
	FILE* fpw = stdout;
	encrypt_hdr_t eh;
	encrypt_blk_hdr_t ebh;
	tb_uint32 rl;
	tb_int32 ret = -1;

	if('-' != zc->file_out[0] || '\0' != zc->file_out[1])
	{
		fpw = fopen(zc->file_out, "wb");
		if(NULL == fpw)
		{
			fprintf(stderr, "Can not open file: %s\n", zc->file_out);
			goto lbl_exit;
		}
	}

	rl = (tb_uint32)fread(&eh, 1, sizeof(eh), zc->fp_in);
	if(rl != (tb_uint32)sizeof(eh))
	{
		fprintf(stderr, "Read encrypt header error!\n");
		goto lbl_exit;
	}

	if(h2nl(eh.magic) != XML_HDR_MAGIC)
	{
		fprintf(stderr, "Invalid encrypt header magic!\n");
		goto lbl_exit;
	}

	rl = (tb_uint32)fread(&ebh, 1, sizeof(ebh), zc->fp_in);
	if(rl != (tb_uint32)sizeof(ebh))
	{
		fprintf(stderr, "Read encrypt block header error!\n");
		goto lbl_exit;
	}

	ebh.cipher_len = h2nl(ebh.cipher_len);
	ebh.plain_len = h2nl(ebh.plain_len);

	if(ebh.plain_len > ebh.cipher_len)
	{
		fprintf(stderr, "Invalid encrypt block header!\n");
		goto lbl_exit;
	}

	pi = (tb_uint8*)malloc(ebh.cipher_len);
	po = (tb_uint8*)malloc(ebh.cipher_len);
	if(NULL == pi || NULL == po)
	{
		fprintf(stderr, "Alloc memory err! len: %u\n", ebh.cipher_len);
		goto lbl_exit;
	}

	rl = (tb_uint32)fread(pi, 1, ebh.cipher_len, zc->fp_in);
	if(rl != ebh.cipher_len)
	{
		fprintf(stderr, "Read encrypt block error!\n");
		goto lbl_exit;
	}

	po[0] = '\0';
	if(tb_aes_decrypt_cbc(pi, ebh.cipher_len, po,
		zc->aes_key_ext, 256, zc->aes_iv) == 0 &&
		(rl = (tb_uint32)strlen((char *)po)) == ebh.plain_len)
	{
		fwrite(po, ebh.plain_len, 1, fpw);
		ret = 0;
		printf("Decrypt ok!\n");
	}
	else
		printf("Decrypt error! plain_len:%u, strlen(po):%u\n",
			ebh.plain_len, rl);

lbl_exit:
	if(fpw != NULL && fpw != stdout)
	{
		fclose(fpw);

		if(ret != 0)
			unlink(zc->file_out);
	}

	free(pi);
	free(po);

	return ret;
}

tb_int32 unpack_hardcode(zxcfg_t* zc)
{
	tb_uint32 kl;
	tb_uint8 key[32], iv[32];
	tb_int32 i, ret = -1;

	if(NULL == zc->key_arg)
		zc->key_arg = HC_KEY;

	kl = (tb_uint32)strlen(zc->key_arg);
	if(kl < 65)
	{
		fprintf(stderr, "Invalid hardcode key!\n");
		goto lbl_exit;
	}

	for(i=0; i<32; i++)
		zc->iv[i] = zc->key_arg[22 + i] + 1;

	for(i=0; i<16; i++)
		zc->key[i] = zc->key_arg[18 + i] + 3;

	kl -= 64;
	i = min(kl, sizeof(zc->key) - 17);
	memcpy(&zc->key[16], &zc->key_arg[64], i);

	tb_sha256(zc->key, i + 16, key);
	tb_sha256(zc->iv, 32, iv);
	tb_aes_key_setup(key, zc->aes_key_ext, 256);
	memcpy(zc->aes_iv, iv, 16);
	ret = decrypt_hc(zc);

lbl_exit:
	return ret;
}

int main(int argc, char* argv[])
{
	zxcfg_t zc;
	tb_int32 ret;

	fprintf(stderr, "Author: yuleniwo\n");
	fprintf(stderr, "SourceCode: https://github.com/yuleniwo/zxcfg\n");
	fprintf(stderr, "Version: %s\n\n", ZXCFG_VER);

	memset(&zc, 0, sizeof(zc));
	zc.cfgtype = CFG_HDR_CFGTYPE;
	zc.defcfgtype = CFG_HDR_DEFCFGTYPE;

	ret = proc_args(&zc, argc, argv);
	if(ret != 0)
		goto lbl_exit;

	if(0 == zc.mode)
		ret = unpack(&zc);
	else if(zc.mode <= 2)
		ret = pack(&zc, (tb_bool)(zc.mode - 1));
	else
		ret = unpack_hardcode(&zc);

	if(zc.fp_in != NULL)
		fclose(zc.fp_in);

lbl_exit:
	return ret;
}
