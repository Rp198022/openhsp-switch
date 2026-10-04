/*	sw_md5.cpp - a plain RFC 1321 MD5, for the hspinet filemd5 shim.
 *
 *	The Chinese build's start-up check hashes its ships-with files (the
 *	seven DLLs plus the readme/URL stubs) and compares the digests against
 *	constants baked into start.ax; hspinet.dll!_filemd5@16 is the first
 *	thing it calls.  Those constants match the stock cards files byte for
 *	byte, so a correct hash is all the check needs.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct {
	uint32_t h[4];
	uint64_t total;
	unsigned char buf[64];
	size_t n;
} swmd5;

static const uint32_t swmd5_k[64] = {
	0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
	0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
	0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
	0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
	0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
	0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
	0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
	0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };

static const int swmd5_r[64] = {
	7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
	5,9,14,20, 5,9,14,20, 5,9,14,20, 5,9,14,20,
	4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
	6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21 };

static uint32_t swmd5_rol( uint32_t x, int c ) { return ( x << c ) | ( x >> ( 32 - c ) ); }

static void swmd5_block( swmd5 *c, const unsigned char *p )
{
	uint32_t m[16], a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3];
	int i;

	for ( i = 0; i < 16; i++ ) {
		m[i] = (uint32_t)p[i*4] | ( (uint32_t)p[i*4+1] << 8 ) |
			   ( (uint32_t)p[i*4+2] << 16 ) | ( (uint32_t)p[i*4+3] << 24 );
	}
	for ( i = 0; i < 64; i++ ) {
		uint32_t f;
		int g;

		if ( i < 16 ) { f = ( b & d ) | ( ~b & e ); g = i; }
		else if ( i < 32 ) { f = ( e & b ) | ( ~e & d ); g = ( 5 * i + 1 ) & 15; }
		else if ( i < 48 ) { f = b ^ d ^ e; g = ( 3 * i + 5 ) & 15; }
		else { f = d ^ ( b | ~e ); g = ( 7 * i ) & 15; }
		f = f + a + swmd5_k[i] + m[g];
		a = e; e = d; d = b;
		b = b + swmd5_rol( f, swmd5_r[i] );
	}
	c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e;
}

static void swmd5_init( swmd5 *c )
{
	c->h[0] = 0x67452301; c->h[1] = 0xefcdab89;
	c->h[2] = 0x98badcfe; c->h[3] = 0x10325476;
	c->total = 0; c->n = 0;
}

static void swmd5_update( swmd5 *c, const unsigned char *p, size_t len )
{
	c->total += len;
	while ( len > 0 ) {
		size_t take = 64 - c->n;

		if ( take > len ) take = len;
		memcpy( c->buf + c->n, p, take );
		c->n += take; p += take; len -= take;
		if ( c->n == 64 ) { swmd5_block( c, c->buf ); c->n = 0; }
	}
}

static void swmd5_final( swmd5 *c, unsigned char out[16] )
{
	uint64_t bits = c->total * 8;
	unsigned char pad[72];
	size_t padlen;
	int i;

	padlen = ( c->n < 56 ) ? ( 56 - c->n ) : ( 120 - c->n );
	memset( pad, 0, sizeof pad );
	pad[0] = 0x80;
	for ( i = 0; i < 8; i++ ) pad[padlen + i] = (unsigned char)( bits >> ( 8 * i ) );
	swmd5_update( c, pad, padlen + 8 );
	for ( i = 0; i < 4; i++ ) {
		out[i*4]   = (unsigned char)( c->h[i] );
		out[i*4+1] = (unsigned char)( c->h[i] >> 8 );
		out[i*4+2] = (unsigned char)( c->h[i] >> 16 );
		out[i*4+3] = (unsigned char)( c->h[i] >> 24 );
	}
}

/*	Hash a file on the card.  Returns 0 with a 32-lowercase-hex digest and
	the byte size on success, -1 when the file cannot be read.			*/
int sw_md5_file( const char *path, char out[33], unsigned int *outsize )
{
	unsigned char chunk[4096], digest[16];
	unsigned long long total = 0;
	swmd5 c;
	FILE *fp;
	size_t got;
	int i;
	static const char hex[] = "0123456789abcdef";

	fp = fopen( path, "rb" );
	if ( fp == NULL ) return -1;
	swmd5_init( &c );
	while ( ( got = fread( chunk, 1, sizeof chunk, fp ) ) > 0 ) {
		swmd5_update( &c, chunk, got );
		total += got;
	}
	fclose( fp );
	swmd5_final( &c, digest );
	for ( i = 0; i < 16; i++ ) {
		out[i*2] = hex[digest[i] >> 4];
		out[i*2+1] = hex[digest[i] & 15];
	}
	out[32] = 0;
	if ( outsize != NULL ) *outsize = (unsigned int)total;
	return 0;
}
