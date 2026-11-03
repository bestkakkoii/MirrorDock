/*
 * Secure Remote Password 6a implementation
 * Copyright (c) 2010 Tom Cocagne. All rights reserved.
 * https://github.com/cocagne/csrp
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013 Tom Cocagne
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *===========================================================================
 * updated (2023) by fduncanh to replace deprecated openssl SHA* hash functions
 * modified (2023) by fduncanh for use with Apple's pair-setup-pin protocol
 */
# include <Windows.h>
# include <Wincrypt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include "srp.h"

static int	g_initialized = 0;

typedef struct
{
    BIGNUM     * N;
    BIGNUM     * g;
} NGConstant;

struct NGHex
{
    const char * n_hex;
    const char * g_hex;
};

/* All constants here were pulled from Appendix A of RFC 5054 */
static struct NGHex global_Ng_constants[] = {
 { /* 2048 */
   "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
   "A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
   "95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
   "747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
   "8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
   "60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
   "FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73",
   "2"
 },
 {0,0} /* null sentinel */
};


static NGConstant * new_ng( SRP_NGType ng_type, const char * n_hex, const char * g_hex )
{
    NGConstant * ng   = (NGConstant *) malloc( sizeof(NGConstant) );
    if (!ng) {
        printf("Memory allocation failure (ng)\n");
        exit(1);
    }
    ng->N             = BN_new();
    ng->g             = BN_new();

    if( !ng || !ng->N || !ng->g )
       return 0;

    if ( ng_type != SRP_NG_CUSTOM )
    {
        int idx = ng_type;
        if ( ng_type > SRP_NG_CUSTOM )
           idx -= 1;
        n_hex = global_Ng_constants[ idx ].n_hex;
        g_hex = global_Ng_constants[ idx ].g_hex;
    }
    BN_hex2bn( &ng->N, n_hex );
    BN_hex2bn( &ng->g, g_hex );

    return ng;
}

static void delete_ng( NGConstant * ng )
{
   if (ng)
   {
      BN_free( ng->N );
      BN_free( ng->g );
      ng->N = 0;
      ng->g = 0;
      free(ng);
   }
}

typedef struct HashCTX_s {
   EVP_MD_CTX *digest_ctx;
} HashCTX_t;


struct SRPVerifier
{
    SRP_HashAlgorithm  hash_alg;
    NGConstant        *ng;
    const char          * username;
    const unsigned char * bytes_B;
    int                   authenticated;
    int                   rfc5054;
    unsigned char M           [SHA512_DIGEST_LENGTH];
    unsigned char H_AMK       [SHA512_DIGEST_LENGTH];
    unsigned char session_key [2 * SHA512_DIGEST_LENGTH];
};

static void handle_error(const char* location) {
    long error = ERR_get_error();
    const char* error_str = ERR_error_string(error, NULL);
    fprintf(stderr, "SRP error at %s: %s\n", location, error_str);
    exit(EXIT_FAILURE);
}

static void hash_destroy( HashCTX_t *ctx) {
    if (ctx) {
        EVP_MD_CTX_free(ctx->digest_ctx);
        free(ctx);
    }
}

static HashCTX_t *hash_create()
{
  HashCTX_t *ctx = (HashCTX_t *) malloc(sizeof(HashCTX_t));
  assert(ctx != NULL);
  ctx->digest_ctx = EVP_MD_CTX_new();
  assert(ctx->digest_ctx != NULL);
  return ctx;
}
		    
int hash_reset( HashCTX_t *ctx)
{
  return EVP_MD_CTX_reset( ctx->digest_ctx); 
}

static int hash_init( SRP_HashAlgorithm alg, HashCTX_t *ctx)
{
  int ret;
  switch (alg)
    {
      case SRP_SHA1  :
        ret = EVP_DigestInit_ex(ctx->digest_ctx, EVP_sha1(), NULL);
        break;
      case SRP_SHA224  :
        ret = EVP_DigestInit_ex(ctx->digest_ctx, EVP_sha224(), NULL);
        break;
      case SRP_SHA256  :
        ret = EVP_DigestInit_ex(ctx->digest_ctx, EVP_sha256(), NULL);
        break;
      case SRP_SHA384  :
        ret = EVP_DigestInit_ex(ctx->digest_ctx, EVP_sha384(), NULL);
        break;
      case SRP_SHA512  :
        ret = EVP_DigestInit_ex(ctx->digest_ctx, EVP_sha512(), NULL);
        break;
      default:
        return -1;
    }
    return ret;    
}

static int hash_update( SRP_HashAlgorithm alg, HashCTX_t *ctx, const void *data, size_t len )
{
    int ret = EVP_DigestUpdate(ctx->digest_ctx, data, len);
    if (!ret) {  
        handle_error(__func__);
    }
    return ret;
}

static int hash_final( SRP_HashAlgorithm alg, HashCTX_t *ctx, unsigned char *md, unsigned int *md_len )
{
    int ret = EVP_DigestFinal_ex(ctx->digest_ctx, md, md_len);
    if (!ret) {
        handle_error(__func__);
    }
    return  ret;
}

static unsigned char * hash( SRP_HashAlgorithm alg, const unsigned char *d, size_t n, unsigned char *md )
{
    switch (alg)
    {
      case SRP_SHA1  : return SHA1( d, n, md );
      case SRP_SHA224: return SHA224( d, n, md );
      case SRP_SHA256: return SHA256( d, n, md );
      case SRP_SHA384: return SHA384( d, n, md );
      case SRP_SHA512: return SHA512( d, n, md );
      default:
        return 0;
    };
}

static int hash_length( SRP_HashAlgorithm alg )
{
    switch (alg)
    {
      case SRP_SHA1  : return SHA_DIGEST_LENGTH;
      case SRP_SHA224: return SHA224_DIGEST_LENGTH;
      case SRP_SHA256: return SHA256_DIGEST_LENGTH;
      case SRP_SHA384: return SHA384_DIGEST_LENGTH;
      case SRP_SHA512: return SHA512_DIGEST_LENGTH;
      default:
        return -1;
    };
}

static BIGNUM * H_nn_orig( SRP_HashAlgorithm alg, const BIGNUM * n1, const BIGNUM * n2 )
{
    unsigned char   buff[ SHA512_DIGEST_LENGTH ];
    int             len_n1 = BN_num_bytes(n1);
    int             len_n2 = BN_num_bytes(n2);
    int             nbytes = len_n1 + len_n2;
    unsigned char * bin    = (unsigned char *) malloc( nbytes );
    if (!bin)
       return 0;
    BN_bn2bin(n1, bin);
    BN_bn2bin(n2, bin + len_n1);
    hash( alg, bin, nbytes, buff );
    free(bin);
    return BN_bin2bn(buff, hash_length(alg), NULL);
}

static BIGNUM * H_nn_rfc5054( SRP_HashAlgorithm alg, const BIGNUM * N, const BIGNUM * n1, const BIGNUM * n2 )
{
    unsigned char   buff[ SHA512_DIGEST_LENGTH ];
    int             len_N  = BN_num_bytes(N);
    int             len_n1 = BN_num_bytes(n1);
    int             len_n2 = BN_num_bytes(n2);
    int             nbytes = len_N * 2;
    unsigned char * bin    = (unsigned char *) malloc( nbytes );
    if (!bin)
       return 0;

    if (len_n1 > len_N || len_n2 > len_N) {
        free (bin);
        return 0;
    }
    
    memset(bin, 0, nbytes);
    BN_bn2bin(n1, bin + (len_N - len_n1));
    BN_bn2bin(n2, bin + (len_N + len_N - len_n2));
    hash( alg, bin, nbytes, buff );
    free(bin);
    return BN_bin2bn(buff, hash_length(alg), NULL);
}

static BIGNUM * H_ns( SRP_HashAlgorithm alg, const BIGNUM * n, const unsigned char * bytes, int len_bytes )
{
    unsigned char   buff[ SHA512_DIGEST_LENGTH ];
    int             len_n  = BN_num_bytes(n);
    int             nbytes = len_n + len_bytes;
    unsigned char * bin    = (unsigned char *) malloc( nbytes );
    if (!bin)
       return 0;
    BN_bn2bin(n, bin);
    memcpy( bin + len_n, bytes, len_bytes );
    hash( alg, bin, nbytes, buff );
    free(bin);
    return BN_bin2bn(buff, hash_length(alg), NULL);
}

static BIGNUM * calculate_x( SRP_HashAlgorithm alg, const BIGNUM * salt, const char * username, const unsigned char * password, int password_len )
{
    unsigned char ucp_hash[SHA512_DIGEST_LENGTH];
    unsigned int ucp_hash_len;
    HashCTX_t *ctx = hash_create();

    hash_init( alg, ctx);
    hash_update( alg, ctx, username, strlen(username) );
    hash_update( alg, ctx, ":", 1 );
    hash_update( alg, ctx, password, password_len );
    hash_final( alg, ctx, ucp_hash, &ucp_hash_len );
    hash_destroy ( ctx);

    return H_ns( alg, salt, ucp_hash, hash_length(alg) );
}

static void update_hash_n( SRP_HashAlgorithm alg, HashCTX_t *ctx, const BIGNUM * n )
{
    unsigned long len = BN_num_bytes(n);
    unsigned char * n_bytes = (unsigned char *) malloc( len );
    if (!n_bytes)
       return;
    BN_bn2bin(n, n_bytes);
    hash_update(alg, ctx, n_bytes, len);
    free(n_bytes);
}

static void hash_num( SRP_HashAlgorithm alg, const BIGNUM * n, unsigned char * dest )
{
    int             nbytes = BN_num_bytes(n);
    unsigned char * bin    = (unsigned char *) malloc( nbytes );
    if(!bin)
       return;
    BN_bn2bin(n, bin);
    hash( alg, bin, nbytes, dest );
    free(bin);
}

/* added for compatibility with Apple's modified srp
 * see https://htmlpreview.github.io/?https://github.com/philippe44/RAOP-Player/blob/master/doc/auth_protocol.html
 */
  
static int hash_session_key( SRP_HashAlgorithm alg, const BIGNUM * n, unsigned char * dest )
{
    HashCTX_t *ctx;
    int             nbytes = BN_num_bytes(n);
    unsigned char * bin    = (unsigned char *) malloc( nbytes );
    unsigned char fourbytes[4] = { 0 };  //Apple's modified SRP protocol
    unsigned int len1, len2;
    if(!bin)
       return -1;
    BN_bn2bin(n, bin);
    
    ctx = hash_create();
    hash_init(alg, ctx);
    hash_update( alg, ctx, bin, nbytes);
    hash_update( alg, ctx, fourbytes, sizeof(fourbytes));
    hash_final( alg, ctx, dest, &len1);
    hash_reset( ctx);

    fourbytes[3] = 1;
    hash_init(alg, ctx);
    hash_update( alg, ctx, bin, nbytes);
    hash_update( alg, ctx, fourbytes, sizeof(fourbytes));
    hash_final( alg, ctx, dest + len1, &len2);
    hash_destroy( ctx);

    free(bin);
    return len1 + len2;
}

static void calculate_M( SRP_HashAlgorithm alg, NGConstant *ng, unsigned char * dest, const char * I, const BIGNUM * s,
                         const BIGNUM * A, const BIGNUM * B, const unsigned char * K )
{
    unsigned char H_N[ SHA512_DIGEST_LENGTH ] = {0};
    unsigned char H_g[ SHA512_DIGEST_LENGTH ] = {0};
    unsigned char H_I[ SHA512_DIGEST_LENGTH ] = {0};
    unsigned char H_xor[ SHA512_DIGEST_LENGTH ] = {0};
    unsigned int dest_len;
    HashCTX_t *ctx;
    int           i = 0;
    int           hash_len = hash_length(alg);

    hash_num( alg, ng->N, H_N );
    hash_num( alg, ng->g, H_g );
    hash(alg, (const unsigned char *)I, strlen(I), H_I);

    for (i=0; i < hash_len; i++ )
        H_xor[i] = H_N[i] ^ H_g[i];

    ctx = hash_create();
    hash_init( alg, ctx);
    hash_update( alg, ctx, H_xor, hash_len );
    hash_update( alg, ctx, H_I,   hash_len );
    update_hash_n( alg, ctx, s );
    update_hash_n( alg, ctx, A );
    update_hash_n( alg, ctx, B );
    hash_update( alg, ctx, K, 2 * hash_len );
    hash_final( alg, ctx, dest, &dest_len );
    hash_destroy ( ctx);
    
}

static void calculate_H_AMK( SRP_HashAlgorithm alg, unsigned char *dest, const BIGNUM * A, const unsigned char * M, const unsigned char * K )
{
    HashCTX_t *ctx;
    unsigned int dest_len;

    ctx = hash_create();
    hash_init( alg, ctx);
    update_hash_n( alg, ctx, A );
    hash_update( alg, ctx, M, hash_length(alg) );
    hash_update( alg, ctx, K, 2 * hash_length(alg) );
    hash_final( alg, ctx, dest, &dest_len );
    hash_destroy ( ctx);
}



static void init_random()
{
    if (g_initialized)
        return;

    HCRYPTPROV wctx;
    unsigned char buff[64];

        CryptAcquireContext(&wctx, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
        CryptGenRandom(wctx, sizeof(buff), (BYTE*) buff);
        CryptReleaseContext(wctx, 0);
        g_initialized = 1;
    if (g_initialized)
       RAND_seed( buff, sizeof(buff) );
}


/***********************************************************************************************************
 *
 *  Exported Functions
 *
 ***********************************************************************************************************/

void srp_random_seed( const unsigned char * random_data, int data_length )
{
    g_initialized = 1;

    if (random_data)
        RAND_seed( random_data, data_length );
}


void srp_create_salted_verification_key( SRP_HashAlgorithm alg,
                                         SRP_NGType ng_type, const char * username,
                                         const unsigned char * password, int len_password,
                                         const unsigned char ** bytes_s, int * len_s,
                                         const unsigned char ** bytes_v, int * len_v,
                                         const char * n_hex, const char * g_hex )
{
    BIGNUM     * s   = BN_new();
    BIGNUM     * v   = BN_new();
    BIGNUM     * x   = 0;
    BN_CTX     * ctx = BN_CTX_new();
    NGConstant * ng  = new_ng( ng_type, n_hex, g_hex );

    if( !s || !v || !ctx || !ng )
       goto cleanup_and_exit;

    init_random(); /* Only happens once */

    BN_rand(s, 128, -1, 0);
    x = calculate_x( alg, s, username, password, len_password );

    if( !x )
       goto cleanup_and_exit;

    BN_mod_exp(v, ng->g, x, ng->N, ctx);

    *len_s   = BN_num_bytes(s);
    *len_v   = BN_num_bytes(v);

    *bytes_s = (const unsigned char *) malloc( *len_s );
    *bytes_v = (const unsigned char *) malloc( *len_v );

    if (!bytes_s || !bytes_v)
       goto cleanup_and_exit;

    BN_bn2bin(s, (unsigned char *) *bytes_s);
    BN_bn2bin(v, (unsigned char *) *bytes_v);

 cleanup_and_exit:
    delete_ng( ng );
    BN_free(s);
    BN_free(v);
    BN_free(x);
    BN_CTX_free(ctx);
}

/* Out: bytes_B, len_B, bytes_b, len_b 
 * On failure, bytes_B and bytes_b  will be set to NULL 
 * len_B  and len_b will be set to 0
 */
void srp_create_server_ephemeral_key( SRP_HashAlgorithm alg, SRP_NGType ng_type,
                                      const unsigned char * bytes_v, int len_v,  
                                      const unsigned char * bytes_b, int len_b,
                                      const unsigned char ** bytes_B, int * len_B,
                                      const char * n_hex, const char * g_hex,
                                      int rfc5054_compat ) {
  BIGNUM             *v    = BN_bin2bn(bytes_v, len_v, NULL);
  BIGNUM             *tmp1 = BN_new();
  BIGNUM             *tmp2 = BN_new();
  BIGNUM             *B    = BN_new();
  BIGNUM             *b    = BN_new();
  BIGNUM             *k    = 0;
  BN_CTX             *ctx  = BN_CTX_new();
  NGConstant         *ng   = new_ng( ng_type, n_hex, g_hex );

  *len_B   = 0;
  *bytes_B = 0;

  if( !v || !B || !b || !tmp1 || !tmp2 || !ctx || !ng )
    goto cleanup_and_exit;

  BN_bin2bn(bytes_b, len_b, b);
  
  if (rfc5054_compat)
    k = H_nn_rfc5054(alg, ng->N, ng->N, ng->g);
  else
    k = H_nn_orig(alg, ng->N, ng->g);
  
  if(!k)
    goto cleanup_and_exit;

  /* B = kv + g^b */
  if (rfc5054_compat)
    {
      BN_mod_mul(tmp1, k, v, ng->N, ctx);
      BN_mod_exp(tmp2, ng->g, b, ng->N, ctx);
      BN_mod_add(B, tmp1, tmp2, ng->N, ctx);
    }
  else
    {
      BN_mul(tmp1, k, v, ctx);
      BN_mod_exp(tmp2, ng->g, b, ng->N, ctx);
      BN_add(B, tmp1, tmp2);
    }

  *len_B   = BN_num_bytes(B);
  *bytes_B = (const unsigned char *)malloc( *len_B );
   BN_bn2bin( B, (unsigned char *) *bytes_B );  

 cleanup_and_exit:
   delete_ng( ng );
   BN_free(v);
   if (k) BN_free(k);
   BN_free(B);
   BN_free(b);
   BN_free(tmp1);
   BN_free(tmp2);
   BN_CTX_free(ctx);
}

/* Out: bytes_B, len_B.
 *
 * On failure, bytes_B will be set to NULL and len_B will be set to 0
 */
struct SRPVerifier *  srp_verifier_new( SRP_HashAlgorithm alg, SRP_NGType ng_type, const char * username,
                                        const unsigned char * bytes_s, int len_s,
                                        const unsigned char * bytes_v, int len_v,
                                        const unsigned char * bytes_A, int len_A,
					const unsigned char * bytes_b, int len_b,
                                        const unsigned char ** bytes_B, int * len_B,
                                        const char * n_hex, const char * g_hex,
                                        int rfc5054_compat )
{
    BIGNUM             *s    = BN_bin2bn(bytes_s, len_s, NULL);
    BIGNUM             *v    = BN_bin2bn(bytes_v, len_v, NULL);
    BIGNUM             *A    = BN_bin2bn(bytes_A, len_A, NULL);
    BIGNUM             *u    = 0;
    BIGNUM             *B    = BN_new();
    BIGNUM             *S    = BN_new();
    BIGNUM             *b    = BN_new();
    BIGNUM             *k    = 0;
    BIGNUM             *tmp1 = BN_new();
    BIGNUM             *tmp2 = BN_new();
    BN_CTX             *ctx  = BN_CTX_new();
    int                 ulen = strlen(username) + 1;
    NGConstant         *ng   = new_ng( ng_type, n_hex, g_hex );
    struct SRPVerifier *ver  = 0;

    *len_B   = 0;
    *bytes_B = 0;

    if( !s || !v || !A || !B || !S || !b || !tmp1 || !tmp2 || !ctx || !ng )
       goto cleanup_and_exit;

    ver = (struct SRPVerifier *) malloc( sizeof(struct SRPVerifier) );

    if (!ver)
       goto cleanup_and_exit;

    init_random(); /* Only happens once */

    ver->username = (char *) malloc( ulen );
    ver->hash_alg = alg;
    ver->ng       = ng;

    if (!ver->username)
    {
       free(ver);
       ver = 0;
       goto cleanup_and_exit;
    }

    memcpy( (char*)ver->username, username, ulen );

    ver->authenticated = 0;
    ver->rfc5054 = rfc5054_compat;

    /* SRP-6a safety check */
    BN_mod(tmp1, A, ng->N, ctx);
    if ( !BN_is_zero(tmp1) )
    {
       if ( !len_b || !bytes_b) {
       BN_rand(b, 256, -1, 0);
       } else {
          BN_bin2bn(bytes_b, len_b, b);
       }

       if (rfc5054_compat)
          k = H_nn_rfc5054(alg, ng->N, ng->N, ng->g);
       else
          k = H_nn_orig(alg, ng->N, ng->g);

       if(!k)
       {
          free(ver);
          ver = 0;
          goto cleanup_and_exit;
       }

       /* B = kv + g^b */
       if (rfc5054_compat)
       {
          BN_mod_mul(tmp1, k, v, ng->N, ctx);
          BN_mod_exp(tmp2, ng->g, b, ng->N, ctx);
          BN_mod_add(B, tmp1, tmp2, ng->N, ctx);
       }
       else
       {
          BN_mul(tmp1, k, v, ctx);
          BN_mod_exp(tmp2, ng->g, b, ng->N, ctx);
          BN_add(B, tmp1, tmp2);
       }

       if (rfc5054_compat)
          u = H_nn_rfc5054(alg, ng->N, A, B);
       else
          u = H_nn_orig(alg, A, B);

       if(!u)
       {
          free(ver);
          ver = 0;
          goto cleanup_and_exit;
       }

       /* S = (A *(v^u)) ^ b */
       BN_mod_exp(tmp1, v, u, ng->N, ctx);
       BN_mul(tmp2, A, tmp1, ctx);
       BN_mod_exp(S, tmp2, b, ng->N, ctx);

       hash_session_key(alg, S, ver->session_key);
       calculate_M( alg, ng, ver->M, username, s, A, B, ver->session_key );
       calculate_H_AMK( alg, ver->H_AMK, A, ver->M, ver->session_key );

       *len_B   = BN_num_bytes(B);
       *bytes_B = (const unsigned char *)malloc( *len_B );

       if( !((const unsigned char *)*bytes_B) )
       {
          free( (void*) ver->username );
          free( ver );
          ver = 0;
          *len_B = 0;
          goto cleanup_and_exit;
       }

       BN_bn2bin( B, (unsigned char *) *bytes_B );

       ver->bytes_B = *bytes_B;
    } else {
       free(ver);
       ver = 0;
    }

 cleanup_and_exit:
    BN_free(s);
    BN_free(v);
    BN_free(A);
    if (u) BN_free(u);
    if (k) BN_free(k);
    BN_free(B);
    BN_free(S);
    BN_free(b);
    BN_free(tmp1);
    BN_free(tmp2);
    BN_CTX_free(ctx);

    return ver;
}

void srp_verifier_delete( struct SRPVerifier * ver )
{
   if (ver)
   {
      delete_ng( ver->ng );
      free( (char *) ver->username );
      free( (unsigned char *) ver->bytes_B );
      memset(ver, 0, sizeof(*ver));
      free( ver );
   }
}

int srp_verifier_is_authenticated( struct SRPVerifier * ver )
{
    return ver->authenticated;
}

const char * srp_verifier_get_username( struct SRPVerifier * ver )
{
    return ver->username;
}

const unsigned char * srp_verifier_get_session_key( struct SRPVerifier * ver, int * key_length )
{
    if (key_length)
      *key_length = 2 * hash_length( ver->hash_alg );
    return ver->session_key;
}

int srp_verifier_get_session_key_length( struct SRPVerifier * ver )
{
    return 2 * hash_length( ver->hash_alg );
}

/* user_M must be exactly SHA512_DIGEST_LENGTH bytes in size */
void srp_verifier_verify_session( struct SRPVerifier * ver, const unsigned char * user_M, const unsigned char ** bytes_HAMK )
{
    if ( memcmp( ver->M, user_M, hash_length(ver->hash_alg) ) == 0 )
    {
        ver->authenticated = 1;
        *bytes_HAMK = ver->H_AMK;
    }
    else
        *bytes_HAMK = NULL;
}

/*******************************************************************************/
