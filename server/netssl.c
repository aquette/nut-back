/* netssl.c - Interface to OpenSSL for upsd

   Copyright (C)
	2002	Russell Kroll <rkroll@exploits.org>
	2008	Arjen de Korte <adkorte-guest@alioth.debian.org>

   based on the original implementation:

   Copyright (C) 2002  Technorama Ltd. <oss-list-ups@technorama.net>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "upsd.h"
#include "neterr.h"
#include "netssl.h"

#ifdef WITH_NSS
	#include <pk11pub.h>
	#include <prinit.h>
	#include <private/pprio.h>
	#include <key.h>
	#include <keyt.h>
	#include <secerr.h>
#endif /* WITH_NSS */


/* stubs for non-ssl compiles */
void net_starttls(nut_ctype_t *client, int numarg, const char **arg)
{
	send_err(client, NUT_ERR_FEATURE_NOT_SUPPORTED);
	return;
}

int ssl_write(nut_ctype_t *client, const char *buf, size_t buflen)
{
	upslogx(LOG_ERR, "ssl_write called but SSL wasn't compiled in");
	return -1;
}

int ssl_read(nut_ctype_t *client, char *buf, size_t buflen)
{
	upslogx(LOG_ERR, "ssl_read called but SSL wasn't compiled in");
	return -1;
}
char	*certfile = NULL;
char	*certpasswd = NULL;

int	ssl_initialized = 0;

void ssl_finish(nut_ctype_t *client)
{
	if (client->ssl) {
		upslogx(LOG_ERR, "ssl_finish found active SSL connection but SSL wasn't compiled in");
	}
}
#ifdef WITH_SSL

#ifdef WITH_OPENSSL

static SSL_CTX	*ssl_ctx = NULL;

static void ssl_debug(void)
{
	int	e;
	char	errmsg[SMALLBUF];

	while ((e = ERR_get_error()) != 0) {
		ERR_error_string_n(e, errmsg, sizeof(errmsg));
		upsdebugx(1, "ssl_debug: %s", errmsg);
	}
}

static int ssl_error(SSL *ssl, int ret)
{
	int	e;

	e = SSL_get_error(ssl, ret);

	switch (e)
	{
	case SSL_ERROR_WANT_READ:
		upsdebugx(1, "ssl_error() ret=%d SSL_ERROR_WANT_READ", ret);
		break;

	case SSL_ERROR_WANT_WRITE:
		upsdebugx(1, "ssl_error() ret=%d SSL_ERROR_WANT_WRITE", ret);
		break;

	case SSL_ERROR_SYSCALL:
		if (ret == 0 && ERR_peek_error() == 0) {
			upsdebugx(1, "ssl_error() EOF from client");
		} else {
			upsdebugx(1, "ssl_error() ret=%d SSL_ERROR_SYSCALL", ret);
		}
		break;

	default:
		upsdebugx(1, "ssl_error() ret=%d SSL_ERROR %d", ret, e);
		ssl_debug();
	}

	return -1;
}

#elif defined(WITH_NSS) /* WITH_OPENSSL */

static CERTCertificate *cert;
static SECKEYPrivateKey *privKey;


static char *nss_password_callback(PK11SlotInfo *slot, PRBool retry, 
		void *arg)
{
	upslogx(LOG_INFO, "nss_password_callback : slot=%s - token=%s - retry=%s\n",
		PK11_GetSlotName(slot), PK11_GetTokenName(slot), retry?"YES":"NO");
	
	if (certpasswd != NULL) {
		return PL_strdup(certpasswd);
	} else {
		return NULL;
	}
}

static void nss_error(const char* text)
{
	char buffer[SMALLBUF];
	PRInt32 length;
	PRErrorCode code = PR_GetError();
	
	switch(code)
	{
	case SEC_ERROR_BAD_PASSWORD:
		upslogx(LOG_ERR, "nss_error : The password entered is incorrect.");
		break;
	default:
		length = PR_GetErrorText(buffer);
		if (length > 0 && length < SMALLBUF) {
			upsdebugx(1, "nss_error %ld in %s : %s", (long)code,
					text, buffer);
		}else{
			upsdebugx(1, "nss_error %ld in %s\n", (long)code,
					text);
		}
	}
}


static int ssl_error(PRFileDesc *ssl, int ret)
{
	char buffer[256];
	PRInt32 length;
	PRErrorCode e;
	
	e = PR_GetError();
	length = PR_GetErrorText(buffer);
	if (length > 0 && length < 256) {
		upsdebugx(1, "ssl_error() ret=%d %*s", e, length, buffer);
	}

	return -1;
}

static SECStatus AuthCertificate(CERTCertDBHandle *arg, PRFileDesc *fd,
	PRBool checksig, PRBool isServer)
{
	upslogx(LOG_DEBUG, "AuthCertificate\n");
	return SSL_AuthCertificate(arg, fd, checksig, isServer);
}

static SECStatus BadCertHandler(ctype_t *arg, PRFileDesc *fd)
{
	upslogx(LOG_DEBUG, "BadCertHandler\n");
	return SECSuccess;
}

static SECStatus GetClientAuthData(ctype_t *arg, PRFileDesc *fd,
	CERTDistNames *caNames, CERTCertificate **pRetCert, SECKEYPrivateKey **pRetKey)
{
	upslogx(LOG_DEBUG, "GetClientAuthData\n");
	return NSS_GetClientAuthData(arg, fd, caNames, pRetCert, pRetKey);
}

static void HandshakeCallback(PRFileDesc *fd, ctype_t *client_data)
{
	upslogx(LOG_DEBUG, "HandshakeCallback\n");
}


#endif /* WITH_OPENSSL | WITH_NSS */


void ssl_init()
{
#if OPENSSL_VERSION_NUMBER >= 0x10000000L
	const SSL_METHOD	*ssl_method;
#else
	SSL_METHOD	*ssl_method;
#endif
#ifdef WITH_NSS
	SECStatus status;
#endif /* WITH_NSS */

	if (!certfile) {
		return;
	}

	check_perms(certfile);

#ifdef WITH_OPENSSL

	SSL_load_error_strings();
	SSL_library_init();

	if ((ssl_method = TLSv1_server_method()) == NULL) {
		ssl_debug();
		fatalx(EXIT_FAILURE, "TLSv1_server_method failed");
	}

	if ((ssl_ctx = SSL_CTX_new(ssl_method)) == NULL) {
		ssl_debug();
		fatalx(EXIT_FAILURE, "SSL_CTX_new failed");
	}

	if (SSL_CTX_use_certificate_chain_file(ssl_ctx, certfile) != 1) {
		ssl_debug();
		fatalx(EXIT_FAILURE, "SSL_CTX_use_certificate_chain_file(%s) failed", certfile);
	}

	if (SSL_CTX_use_PrivateKey_file(ssl_ctx, certfile, SSL_FILETYPE_PEM) != 1) {
		ssl_debug();
		fatalx(EXIT_FAILURE, "SSL_CTX_use_PrivateKey_file(%s) failed", certfile);
	}

	if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
		ssl_debug();
		fatalx(EXIT_FAILURE, "SSL_CTX_check_private_key(%s) failed", certfile);
	}

	if (SSL_CTX_set_cipher_list(ssl_ctx, "HIGH:@STRENGTH") != 1) {
		ssl_debug();
		fatalx(EXIT_FAILURE, "SSL_CTX_set_cipher_list failed");
	}

	SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);

	ssl_initialized = 1;
		
#elif defined(WITH_NSS) /* WITH_OPENSSL */

	PR_Init(PR_USER_THREAD, PR_PRIORITY_NORMAL, 0);
	
	PK11_SetPasswordFunc(nss_password_callback);
	
	if (certfile)
		/* Note: this call can generate memory leaks not resolvable
		 * by any release function.
		 * Probably NSS key module object allocation and
		 * probably NSS key db object allocation too. */
		status = NSS_Init(certfile);
	else
		status = NSS_NoDB_Init(NULL);
	if (status != SECSuccess) {
		nss_error("NSS_Init(certfile)");
		return;
	}
	
	status = NSS_SetDomesticPolicy();
	if (status != SECSuccess) {
		nss_error("NSS_SetDomesticPolicy");
		return;
	}

	/* Default server cache config */
	status = SSL_ConfigServerSessionIDCache(0, 0, 0, NULL);
	if (status != SECSuccess) {
		nss_error("SSL_ConfigServerSessionIDCache");
		return;
	}
	
	status = SSL_OptionSetDefault(SSL_ENABLE_SSL3, PR_TRUE);
	if (status != SECSuccess) {
		nss_error("SSL_OptionSetDefault(SSL_ENABLE_SSL3, PR_TRUE)");
		return;
	}
	status = SSL_OptionSetDefault(SSL_ENABLE_TLS, PR_TRUE);
	if (status != SECSuccess) {
		nss_error("SSL_OptionSetDefault(SSL_ENABLE_TLS, PR_TRUE)");
		return;
	}
	
	cert = PK11_FindCertFromNickname("My nut server", NULL);
	if(cert==NULL)	{
		nss_error("PK11_FindCertFromNickname\n");
		return;
	}
	
	privKey = PK11_FindKeyByAnyCert(cert, NULL);
	if(privKey==NULL){
		nss_error("PK11_FindKeyByAnyCert\n");
		return;
	}
		
	ssl_initialized = 1;
#else /* WITH_OPENSSL | WITH_NSS */
	upslogx(LOG_ERR, "ssl_init called but SSL wasn't compiled in");
#endif /* WITH_OPENSSL | WITH_NSS */
}

void ssl_cleanup(void)
{
#ifdef WITH_OPENSSL
	if (ssl_ctx) {
		SSL_CTX_free(ssl_ctx);
		ssl_ctx = NULL;
	}
#elif defined(WITH_NSS) /* WITH_OPENSSL */
	CERT_DestroyCertificate(cert);
    SECKEY_DestroyPrivateKey(privKey);
	NSS_Shutdown();
	PR_Cleanup();
	/* Called to release memory arena used by NSS/NSPR.
	 * Prevent to show all PL_ArenaAllocate mem alloc as leaks.
	 * https://developer.mozilla.org/en/NSS_Memory_allocation
	 */
	PL_ArenaFinish();
#else /* WITH_OPENSSL | WITH_NSS */
	/* Do nothing */
#endif /* WITH_OPENSSL | WITH_NSS */
	ssl_initialized = 0;
}


void net_starttls(nut_ctype_t *client, int numarg, const char **arg)
{
#ifdef WITH_OPENSSL
	int ret;
#elif defined(WITH_NSS) /* WITH_OPENSSL */
	SECStatus	status;
	PRFileDesc	*socket;
#endif /* WITH_OPENSSL | WITH_NSS */
	
	if (client->ssl) {
		send_err(client, NUT_ERR_ALREADY_SSL_MODE);
		return;
	}

	client->ssl_connected = 0;

	if ((!certfile) || (!ssl_initialized)) {
		send_err(client, NUT_ERR_FEATURE_NOT_CONFIGURED);
		return;
	}
	
#ifdef WITH_OPENSSL	
	if (!ssl_ctx) {
#elif defined(WITH_NSS) /* WITH_OPENSSL */
	if (!NSS_IsInitialized()) {
#endif /* WITH_OPENSSL | WITH_NSS */
		send_err(client, NUT_ERR_FEATURE_NOT_CONFIGURED);
		ssl_initialized = 0;
		return;
	}
	
	if (!sendback(client, "OK STARTTLS\n")) {
		return;
	}

#ifdef WITH_OPENSSL	

	client->ssl = SSL_new(ssl_ctx);

	if (!client->ssl) {
		upslog_with_errno(LOG_ERR, "SSL_new failed\n");
		ssl_debug();
		return;
	}

	if (SSL_set_fd(client->ssl, client->sock_fd) != 1) {
		upslog_with_errno(LOG_ERR, "SSL_set_fd failed\n");
		ssl_debug();
		return;
	}
	
static int ssl_accept(nut_ctype_t *client)
{
	int	ret;

	ret = SSL_accept(client->ssl);
	switch (ret)
	{
	case 1:
		client->ssl_connected = 1;
		upsdebugx(3, "SSL connected");
		break;
		
	case 0:
		upslog_with_errno(LOG_ERR, "SSL_accept do not accept handshake.");
		ssl_error(client->ssl, ret);
		break;
	case -1:
		upslog_with_errno(LOG_ERR, "Unknown return value from SSL_accept");
		ssl_error(client->ssl, ret);
		break;
	}
	
#elif defined(WITH_NSS) /* WITH_OPENSSL */

	socket = PR_ImportTCPSocket(client->sock_fd);
	if (socket == NULL){
		nss_error("PR_ImportTCPSocket");
		return;
	}

	client->ssl = SSL_ImportFD(NULL, socket);
	if (client->ssl == NULL){
		nss_error("SSL_ImportFD");
		return;
	}
	
	if (SSL_SetPKCS11PinArg(client->ssl, client) == -1){
		nss_error("SSL_SetPKCS11PinArg");
		return;
	}
	
	/* Note cast to SSLAuthCertificate to prevent warning due to
	 * bad function prototype in NSS.
	 */
	status = SSL_AuthCertificateHook(client->ssl, (SSLAuthCertificate)AuthCertificate, CERT_GetDefaultCertDB());
	if (status != SECSuccess) {
		nss_error("SSL_AuthCertificateHook");
		return;
	}
	
	status = SSL_BadCertHook(client->ssl, (SSLBadCertHandler)BadCertHandler, client);
	if (status != SECSuccess) {
		nss_error("SSL_BadCertHook");
		return;
	}
	
	status = SSL_GetClientAuthDataHook(client->ssl, (SSLGetClientAuthData)GetClientAuthData, client);
	if (status != SECSuccess) {
		nss_error("SSL_GetClientAuthDataHook");
		return;
	}
	
	status = SSL_HandshakeCallback(client->ssl, (SSLHandshakeCallback)HandshakeCallback, client);
	if (status != SECSuccess) {
		nss_error("SSL_HandshakeCallback");
		return;
	}
	
	status = SSL_ConfigSecureServer(client->ssl, cert, privKey, NSS_FindCertKEAType(cert));
	if (status != SECSuccess) {
		nss_error("SSL_ConfigSecureServer");
		return;
	}

	status = SSL_ResetHandshake(client->ssl, PR_TRUE);
	if (status != SECSuccess) {
		nss_error("SSL_ResetHandshake");
		return;
	}

	/* Note: this call can generate memory leaks not resolvable
	 * by any release function.
	 * Probably SSL session key object allocation. */
	status = SSL_ForceHandshake(client->ssl);
	if (status != SECSuccess) {
		nss_error("SSL_ForceHandshake");
		return;
	}

#endif /* WITH_OPENSSL | WITH_NSS */

	client->ssl_connected = 1;
}

void ssl_finish(ctype_t *client)
{
	if (client->ssl) {
#ifdef WITH_OPENSSL
		SSL_free(client->ssl);
#elif defined(WITH_NSS)
		PR_Shutdown(client->ssl, PR_SHUTDOWN_BOTH);
		PR_Close(client->ssl);
#endif /* WITH_OPENSSL | WITH_NSS */
		client->ssl_connected = 0;
		client->ssl = NULL;
	}
}

int ssl_read(nut_ctype_t *client, char *buf, size_t buflen)
{
	int	ret;

	if (!client->ssl_connected) {
		return -1;
	}

#ifdef WITH_OPENSSL
	ret = SSL_read(client->ssl, buf, buflen);
#elif defined(WITH_NSS) /* WITH_OPENSSL */
	ret = PR_Read(client->ssl, buf, buflen);
#endif /* WITH_OPENSSL | WITH_NSS */

	if (ret < 1) {
		ssl_error(client->ssl, ret);
		return -1;
	}

	return ret;
}

int ssl_write(nut_ctype_t *client, const char *buf, size_t buflen)
{
	int	ret;

	if (!client->ssl_connected) {
		return -1;
	}

#ifdef WITH_OPENSSL
	ret = SSL_write(client->ssl, buf, buflen);
#elif defined(WITH_NSS) /* WITH_OPENSSL */
	ret = PR_Write(client->ssl, buf, buflen);
#endif /* WITH_OPENSSL | WITH_NSS */

	upsdebugx(5, "ssl_write ret=%d", ret);

	return ret;
}

#else /* WITH_SSL */

void ssl_init(void)
{
	ssl_initialized = 0;	/* keep gcc quiet */
}

void ssl_cleanup(void)
{
}

void net_starttls(ctype_t *client, int numarg, const char **arg)
{
	send_err(client, NUT_ERR_FEATURE_NOT_SUPPORTED);
	return;
}

void ssl_finish(nut_ctype_t *client)
{
	if (client->ssl) {
		upslogx(LOG_ERR, "ssl_finish found active SSL connection but SSL wasn't compiled in");
	}
}

int ssl_write(ctype_t *client, const char *buf, size_t buflen)
{
	upslogx(LOG_ERR, "ssl_write called but SSL wasn't compiled in");
	return -1;
}

int ssl_read(ctype_t *client, char *buf, size_t buflen)
{
	upslogx(LOG_ERR, "ssl_read called but SSL wasn't compiled in");
	return -1;
}

#endif /* WITH_SSL */