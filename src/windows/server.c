
/* vim: set et ts=3 sw=3 ft=c:
 *
 * Copyright (C) 2011, 2012 James McLaughlin et al.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "../common.h"
#include "../address.h"
#include "sslclient.h"
#include "fdstream.h"

static void on_client_close (lw_stream, void * tag);
static void on_client_data (lw_stream, void * tag, const char * buffer, size_t size);

struct lw_server
{
   SOCKET socket;

   lw_pump pump;

   lw_server_hook_connect on_connect;
   lw_server_hook_disconnect on_disconnect;
   lw_server_hook_data on_data;
   lw_server_hook_error on_error;

   lw_bool cert_loaded;
   CredHandle ssl_creds;

   long accepts_posted;

   list (lw_server_client, clients);

   void * tag;
};
    
struct lw_server_client
{
   struct lw_fdstream fdstream;

   lw_server server;

   int user_count;

   lw_bool on_connect_called;
   lw_bool dead;

   lwp_winsslclient ssl;

   lw_addr addr;

   HANDLE socket;

   lw_server_client elem;
};

lw_server lw_server_new (lw_pump pump)
{
   lw_server ctx = calloc (sizeof (*ctx), 1);

   if (!ctx)
      return 0;

   lwp_init ();

   ctx->socket = -1;
   ctx->pump = pump;

   return ctx;
}

void lw_server_delete (lw_server ctx)
{
   lw_server_unhost (ctx);

   free (ctx);
}

void lw_server_set_tag (lw_server ctx, void * tag)
{
   ctx->tag = tag;
}

void * lw_server_tag (lw_server ctx)
{
   return ctx->tag;
}

lw_server_client lwp_server_client_new (lw_server ctx, SOCKET socket)
{
   lw_server_client client = calloc (sizeof (*client), 1);

   if (!client)
      return 0;

   client->server = ctx;

   lwp_fdstream_init ((lw_fdstream) client, ctx->pump);

   /* The first added close handler is always the last called.
    * This is important, because ours will destroy the client.
    */

   lw_stream_add_hook_close ((lw_stream) client, on_client_close, client);

   if (ctx->cert_loaded)
   {
      client->ssl = lwp_winsslclient_new (ctx->ssl_creds, (lw_stream) client);
   }

   lw_fdstream_set_fd ((lw_fdstream) client, (HANDLE) socket, 0, lw_true);

   return client;
}

void lwp_server_client_delete (lw_server_client client)
{
   if (!client)
      return;

   lw_server ctx = client->server;

   lwp_trace ("Terminate %p", client);

   ++ client->user_count;

   client->socket = INVALID_HANDLE_VALUE;

   if (client->on_connect_called)
   {
      if (ctx->on_disconnect)
         ctx->on_disconnect (ctx, client);

      list_elem_remove (client->elem);
   }

   lwp_winsslclient_delete (client->ssl);

   free (client);
}

const int ideal_pending_accept_count = 16;

typedef struct 
{
   OVERLAPPED overlapped;

   SOCKET socket;

   struct lw_addr addr;
   char addr_buffer [(sizeof (struct sockaddr_storage) + 16) * 2];

} accept_overlapped;

static lw_bool issue_accept (lw_server ctx)
{
   accept_overlapped * overlapped = calloc (sizeof (*overlapped), 1);

   if (!overlapped)
      return lw_false;

   if ((overlapped->socket = WSASocket (lwp_socket_addr (ctx->socket).ss_family,
                                        SOCK_STREAM,
                                        IPPROTO_TCP,
                                        0,
                                        0,
                                        WSA_FLAG_OVERLAPPED)) == INVALID_SOCKET)
   {
      free (overlapped);
      return lw_false;
   }

   lwp_disable_ipv6_only ((lwp_socket) overlapped->socket);

   DWORD bytes_received; 

   /* TODO : Use AcceptEx to receive the first data? */

   if (!AcceptEx (ctx->socket,
                  overlapped->socket,
                  overlapped->addr_buffer,
                  0,
                  sizeof (struct sockaddr_storage) + 16,
                  sizeof (struct sockaddr_storage) + 16,
                  &bytes_received,
                  (OVERLAPPED *) overlapped))
   {
      int error = WSAGetLastError ();

      if (error != ERROR_IO_PENDING)
         return lw_false;
   }

   ++ ctx->accepts_posted;

   return lw_true;
}

static void listen_socket_completion (void * tag, OVERLAPPED * _overlapped,
                                      unsigned long bytes_transferred, int error)
{
   lw_server ctx = tag;
   accept_overlapped * overlapped = (accept_overlapped *) _overlapped;

   -- ctx->accepts_posted;

   if (error)
   {
      free (overlapped);
      return;
   }

   while (ctx->accepts_posted < ideal_pending_accept_count)
      if (!issue_accept (ctx))
         break;

   setsockopt ((SOCKET) overlapped->socket, SOL_SOCKET,
               SO_UPDATE_ACCEPT_CONTEXT,
               (char *) &ctx->socket, sizeof (ctx->socket));

   struct sockaddr_storage * local_addr, * remote_addr;
   int local_addr_len, remote_addr_len;

   GetAcceptExSockaddrs
   (
      overlapped->addr_buffer,
      0,

      sizeof (struct sockaddr_storage) + 16,
      sizeof (struct sockaddr_storage) + 16,

      (struct sockaddr **) &local_addr,
      &local_addr_len,

      (struct sockaddr **) &remote_addr,
      &remote_addr_len
   );

   lw_server_client client = lwp_server_client_new (ctx, overlapped->socket);

   if (!client)
   {
      closesocket ((SOCKET) overlapped->socket);
      free (overlapped);

      return;
   }

   client->addr = lwp_addr_new_sockaddr ((struct sockaddr *) remote_addr);

   free (overlapped);

   ++ client->user_count;

   if (ctx->on_connect)
      ctx->on_connect (ctx, client);

   if (client->dead)
   {
      lwp_server_client_delete (client);
      return;
   }

   list_push (ctx->clients, client);
   client->elem = list_back (ctx->clients);

   -- client->user_count;

   if (ctx->on_data)
   {
      lwp_trace ("*** READING on behalf of the handler, client %p", client);

      lw_stream_add_hook_data ((lw_stream) client, on_client_data, client);
      lw_stream_read ((lw_stream) client, -1);
   }
}

void lw_server_host (lw_server ctx, long port)
{
   lw_filter filter = lw_filter_new ();
   lw_filter_set_local_port (filter, port);

   lw_server_host_filter (ctx, filter);

   lw_filter_delete (filter);
}

void lw_server_host_filter (lw_server ctx, lw_filter filter)
{
   lw_server_unhost (ctx);

   lw_error error = lw_error_new ();

   if ((ctx->socket = lwp_create_server_socket
            (filter, SOCK_STREAM, IPPROTO_TCP, error)) == -1)
   {
      if (ctx->on_error)
         ctx->on_error (ctx, error);

      return;
   }

   if (listen (ctx->socket, SOMAXCONN) == -1)
   {
      lw_error error = lw_error_new ();

      lw_error_add (error, WSAGetLastError ());
      lw_error_addf (error, "Error listening");

      if (ctx->on_error)
         ctx->on_error (ctx, error);

      lw_error_delete (error);

      return;
   }

   lw_pump_add (ctx->pump, (HANDLE) ctx->socket, ctx, listen_socket_completion);

   while (ctx->accepts_posted < ideal_pending_accept_count)
      if (!issue_accept (ctx))
         break;
}

void lw_server_unhost (lw_server ctx)
{
    if(!lw_server_hosting (ctx))
        return;

    closesocket (ctx->socket);
    ctx->socket = -1;
}

lw_bool lw_server_hosting (lw_server ctx)
{
   return ctx->socket != -1;
}

size_t lw_server_num_clients (lw_server ctx)
{
   return list_length (ctx->clients);
}

long lw_server_port (lw_server ctx)
{
   return lwp_socket_port (ctx->socket);
}

lw_bool lw_server_load_sys_cert (lw_server ctx,
                                 const char * store_name,
                                 const char * common_name,
                                 const char * location)
{
   if (lw_server_hosting (ctx) || lw_server_cert_loaded (ctx))
   {
      lw_error error = lw_error_new ();

      lw_error_addf (error,
            "Either the server is already hosting, or a certificate has already been loaded");

      if (ctx->on_error)
         ctx->on_error (ctx, error);

      lw_error_delete (error);

      return lw_false;
   }

   if(!location || !*location)
      location = "CurrentUser";

   if(!store_name || !*store_name)
      store_name = "MY";

   int location_id = -1;

   do
   {
      if(!strcasecmp (location, "CurrentService"))
      {
         location_id = 0x40000; /* CERT_SYSTEM_STORE_CURRENT_SERVICE */
         break;
      }

      if(!strcasecmp (location, "CurrentUser"))
      {
         location_id = 0x10000; /* CERT_SYSTEM_STORE_CURRENT_USER */
         break;
      }

      if(!strcasecmp (location, "CurrentUserGroupPolicy"))
      {
         location_id = 0x70000; /* CERT_SYSTEM_STORE_CURRENT_USER_GROUP_POLICY */
         break;
      }

      if(!strcasecmp (location, "LocalMachine"))
      {
         location_id = 0x20000; /* CERT_SYSTEM_STORE_LOCAL_MACHINE */
         break;
      }

      if(!strcasecmp (location, "LocalMachineEnterprise"))
      {
         location_id = 0x90000; /* CERT_SYSTEM_STORE_LOCAL_MACHINE_ENTERPRISE */
         break;
      }

      if(!strcasecmp (location, "LocalMachineGroupPolicy"))
      {
         location_id = 0x80000; /* CERT_SYSTEM_STORE_LOCAL_MACHINE_GROUP_POLICY */
         break;
      }

      if(!strcasecmp (location, "Services"))
      {
         location_id = 0x50000; /* CERT_SYSTEM_STORE_SERVICES */
         break;
      }

      if(!strcasecmp (location, "Users"))
      {
         location_id = 0x60000; /* CERT_SYSTEM_STORE_USERS */
         break;
      }

   } while(0);
    
   if (location_id == -1)
   {
      lw_error error = lw_error_new ();

      lw_error_addf (error, "Unknown certificate location: %s", location);
      lw_error_addf (error, "Error loading certificate");

      if (ctx->on_error)
         ctx->on_error (ctx, error);

      return lw_false;
   }

   HCERTSTORE cert_store = CertOpenStore
   (
      (LPCSTR) 9, /* CERT_STORE_PROV_SYSTEM_A */
      0,
      0,
      location_id,
      store_name
   );

   if (!cert_store)
   {
      lw_error error = lw_error_new ();

      lw_error_add (error, WSAGetLastError ());
      lw_error_addf (error, "Error loading certificate");

      if (ctx->on_error)
         ctx->on_error (ctx, error);

      lw_error_delete (error);

      return lw_false;
   }

   PCCERT_CONTEXT context = CertFindCertificateInStore
   (
      cert_store,
      X509_ASN_ENCODING,
      0,
      CERT_FIND_SUBJECT_STR_A,
      common_name,
      0
   );

   if (!context)
   {
      int code = GetLastError();

      context = CertFindCertificateInStore
      (
         cert_store,
         PKCS_7_ASN_ENCODING,
         0,
         CERT_FIND_SUBJECT_STR_A,
         common_name,
         0
      );

      if (!context)
      {
         lw_error error = lw_error_new ();

         lw_error_add (error, code);
         lw_error_addf (error, "Error finding certificate in store");

         if (ctx->on_error)
            ctx->on_error (ctx, error);

         lw_error_delete (error);

         return lw_false;
      }
   }

   SCHANNEL_CRED creds =
   {
      .dwVersion              = SCHANNEL_CRED_VERSION,
      .cCreds                 = 1,
      .paCred                 = &context,
      .grbitEnabledProtocols  = (0x80 | 0x40)  /* SP_PROT_TLS1 */
   };

   {  TimeStamp expiry_time;

      int result = AcquireCredentialsHandleA
      (
         0,
         (SEC_CHAR *) UNISP_NAME_A,
         SECPKG_CRED_INBOUND,
         0,
         &creds,
         0,
         0,
         &ctx->ssl_creds,
         &expiry_time
      );

      if (result != SEC_E_OK)
      {
         lw_error error = lw_error_new ();

         lw_error_add (error, result);
         lw_error_addf (error, "Error acquiring credentials handle");

         if (ctx->on_error)
            ctx->on_error (ctx, error);

         lw_error_delete (error);

         return lw_false;
      }
   }

   ctx->cert_loaded = lw_true;

   return lw_true;
}

lw_bool lw_server_load_cert_file (lw_server ctx,
                                  const char * filename,
                                  const char * common_name)
{
   if (!lw_file_exists (filename))
   {
      lw_error error = lw_error_new ();

      lw_error_addf (error, "File not found: %s", filename);
      lw_error_addf (error, "Error loading certificate");

      if (ctx->on_error)
         ctx->on_error (ctx, error);

      lw_error_delete (error);

      return lw_false;
   }

   if (lw_server_hosting (ctx))
      lw_server_unhost (ctx);

   if(lw_server_cert_loaded (ctx))
   {
      FreeCredentialsHandle (&ctx->ssl_creds);
      ctx->cert_loaded = lw_false;
   }

   HCERTSTORE cert_store = CertOpenStore
   (
      (LPCSTR) 7 /* CERT_STORE_PROV_FILENAME_A */,
      X509_ASN_ENCODING,
      0,
      CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG,
      filename
   );

   lw_bool pkcs7 = lw_false;

   if (!cert_store)
   {
      cert_store = CertOpenStore
      (
         (LPCSTR) 7 /* CERT_STORE_PROV_FILENAME_A */,
         PKCS_7_ASN_ENCODING,
         0,
         CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG,
         filename
      );

      pkcs7 = lw_true;

      if (!cert_store)
      {
         lw_error error = lw_error_new ();

         lw_error_add (error, GetLastError ());
         lw_error_addf (error, "Error loading certificate file: %s", filename);

         if (ctx->on_error)
            ctx->on_error (ctx, error);

         lw_error_delete (error);

         return lw_false;
      }
   }

   PCCERT_CONTEXT context = CertFindCertificateInStore
   (
      cert_store,
      pkcs7 ? PKCS_7_ASN_ENCODING : X509_ASN_ENCODING,
      0,
      CERT_FIND_SUBJECT_STR_A,
      common_name,
      0
   );

   if (!context)
   {
      int code = GetLastError();

      context = CertFindCertificateInStore
      (
         cert_store,
         pkcs7 ? X509_ASN_ENCODING : PKCS_7_ASN_ENCODING,
         0,
         CERT_FIND_SUBJECT_STR_A,
         common_name,
         0
      );

      if (!context)
      {
         lw_error error = lw_error_new ();

         lw_error_add (error, code);
         lw_error_addf (error, "Error finding certificate in store");

         if (ctx->on_error)
            ctx->on_error (ctx, error);

         lw_error_delete (error);

         return lw_false;
      }
   }

   SCHANNEL_CRED creds =
   {
      .dwVersion = SCHANNEL_CRED_VERSION,
      .cCreds = 1,
      .paCred = &context,
      .grbitEnabledProtocols = 0xF0 /* SP_PROT_SSL3TLS1 */
   };

   TimeStamp expiry_time; 

   int result = AcquireCredentialsHandleA
   (
      0,
      (SEC_CHAR *) UNISP_NAME_A,
      SECPKG_CRED_INBOUND,
      0,
      &creds,
      0,
      0,
      &ctx->ssl_creds,
      &expiry_time
   );

   if (result != SEC_E_OK)
   {
      lw_error error = lw_error_new ();

      lw_error_add (error, result);
      lw_error_addf (error, "Error acquiring credentials handle");

      if (ctx->on_error)
         ctx->on_error (ctx, error);

      lw_error_delete (error);

      return lw_false;
   }

   ctx->cert_loaded = lw_true;

   return lw_true;
}

lw_bool lw_server_cert_loaded (lw_server ctx)
{
   return ctx->cert_loaded;   
}

lw_bool lw_server_can_npn (lw_server ctx)
{
   /* NPN is currently not available w/ schannel */

   return lw_false;
}

void lw_server_add_npn (lw_server ctx, const char * protocol)
{
}

const char * lw_server_client_npn (lw_server_client client)
{
   return "";
}

lw_addr lw_server_client_addr (lw_server_client client)
{
   return client->addr;
}

lw_server_client lw_server_client_next (lw_server_client client)
{
   return list_elem_next (client->elem);
}

lw_server_client lw_server_client_first (lw_server ctx)
{
   return list_front (ctx->clients);
}

void on_client_data (lw_stream stream, void * tag, const char * buffer, size_t size)
{
   lw_server_client client = tag;
   lw_server server = client->server;

   assert (server->on_data);

   server->on_data (server, client, buffer, size);
}

void on_client_close (lw_stream stream, void * tag)
{
   lw_server_client client = tag;

   if (client->user_count > 0)
      client->dead = lw_false;
   else
      lwp_server_client_delete (client);
}

void lw_server_on_data (lw_server ctx, lw_server_hook_data on_data)
{
   ctx->on_data = on_data;

   if (on_data)
   {
      /* Setting on_data to a handler */

      if (!ctx->on_data)
      {
         list_each (ctx->clients, client)
         {
            lw_stream_add_hook_data ((lw_stream) client, on_client_data, client);
            lw_stream_read ((lw_stream) client, -1);
         }
      }

      return;
   }

   /* Setting on_data to 0 */

   list_each (ctx->clients, client)
   {
      lw_stream_remove_hook_data ((lw_stream) client, on_client_data, client);
   }
}

lwp_def_hook (server, connect)
lwp_def_hook (server, disconnect)
lwp_def_hook (server, error)
