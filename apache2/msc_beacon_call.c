/*
* ModSecurity for Apache 2.x, http://www.modsecurity.org/
* Copyright (c) 2004-2013 Trustwave Holdings, Inc. (http://www.trustwave.com/)
*
* You may not use this file except in compliance with
* the License.  You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* If any of the files related to licensing are missing or if you have any
* other questions related to licensing please contact Trustwave Holdings, Inc.
* directly using the email address security@modsecurity.org.
*/

#include "msc_beacon_call.h"
#include "apr_sha1.h"

#include <sys/utsname.h>

#ifdef __gnu_linux__
#include <linux/if.h>        
#include <linux/sockios.h>   
#endif            

// Bese32 encode, based on:
// https://code.google.com/p/google-authenticator/source/browse/libpam/base32.c
void DSOLOCAL msc_beacon_call_base32_encode(char *encoded,
    const uint8_t *data, int len)
{
    int i;
    int buffer;
    int count = 0;
    char *result = encoded;

    buffer = data[0];
    int length = strlen(data);

    if (length > 0)
    {
        int next = 1;
        int bitsLeft = 8;
        while (count < len && (bitsLeft > 0 || next < length))
        {
            if (bitsLeft < 5)
            {
                if (next < length)
                {
                    buffer <<= 8;
                    buffer |= data[next++] & 0xff;
                    bitsLeft += 8;
                } else {
                    int pad = 5 - bitsLeft;
                    buffer <<= pad;
                    bitsLeft += pad;
                }
            }
            int index = 0x1f & (buffer >> (bitsLeft - 5));
            bitsLeft -= 5;
            result[count++] = msc_beacon_basis_32[index];
        }
    }
    if (count < len)
    {
        result[count] = '\000';
    }
}

int DSOLOCAL msc_beacon_call_fill_with_dots(char *encoded_with_dots,
    const uint8_t *data, int len, int space)
{
    int i, count = 0;

    for (i = 0; i < strlen(data) && i < len; i++)
    {
        if (i % space == 0 && i != 0)
        {
            encoded_with_dots[count++] = '.';
        }
        encoded_with_dots[count++] = data[i];
    }
    encoded_with_dots[count] = '\0';
}


// Based on:
// http://stackoverflow.com/questions/16858782/how-to-obtain-almost-unique-system-identifier-in-a-cross-platform-way
int DSOLOCAL msc_beacon_call_machine_name(char *machine_name, size_t len)
{
    memset(machine_name, '\0', sizeof(char) * len);

#ifdef WIN32
    static char computerName[1024];
    DWORD size = 1024;
    GetComputerName( computerName, &size );

    snprintf(machine_name, len-1, "%s", computerName[0]);
#else
   static struct utsname u;

   if ( uname( &u ) < 0 )
   {
      goto failed;
   }

   snprintf(machine_name, len-1, "%s\0", u.nodename);
#endif

    return 0;

failed:
    return -1;
}

int DSOLOCAL msc_beacon_call_mac_address (unsigned char *mac)
{
#ifdef DARWIN
    struct ifaddrs* ifaphead;
    if ( getifaddrs( &ifaphead ) != 0 )
    {
        return;
    }

    // iterate over the net interfaces         
    bool foundMac1 = false;
    struct ifaddrs* ifap;
    for ( ifap = ifaphead; ifap; ifap = ifap->ifa_next )
    {
        struct sockaddr_dl* sdl = (struct sockaddr_dl*)ifap->ifa_addr;
        if ( sdl && ( sdl->sdl_family == AF_LINK ) && ( sdl->sdl_type == IFT_ETHER ))
        {
            if ( !foundMac1 )
            {
                foundMac1 = true;
                mac1 = (u8*)(LLADDR(sdl));
            }
            else
            {
                mac2 = (u8*)(LLADDR(sdl));
                break;
            }
        }
    }
    freeifaddrs( ifaphead );
#endif

#if __gnu_linux__
    struct ifconf conf;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP );
    struct ifreq* ifr;
    if ( sock < 0 )
    {
        goto failed;
    }

    char ifconfbuf[ 128 * sizeof(struct ifreq) ];
    memset( ifconfbuf, 0, sizeof( ifconfbuf ));
    conf.ifc_buf = ifconfbuf;
    conf.ifc_len = sizeof( ifconfbuf );
    if ( ioctl( sock, SIOCGIFCONF, &conf ))
    {
        close(sock);
        goto failed;
    }

    for ( ifr = conf.ifc_req; ifr < conf.ifc_req + conf.ifc_len; ifr++ )
    {
        if ( ioctl( sock, SIOCGIFFLAGS, ifr ))
        {
            continue;  // failed to get flags, skip it
        }

        if ( ioctl( sock, SIOCGIFHWADDR, ifr ) == 0 )
        {
            int i = 0;
            if (!ifr->ifr_addr.sa_data[0] && !ifr->ifr_addr.sa_data[1]
                && !ifr->ifr_addr.sa_data[2])
            {
                continue;
            }

            for (i = 0; i<6; i++)
            {
                sprintf(mac, "%s%s%02x", mac, (i == 0)?"":":",
                    (unsigned char)ifr->ifr_addr.sa_data[i]);
            }
        }
    }
    close( sock );
#endif

#if WIN32
    IP_ADAPTER_INFO AdapterInfo[32];
    DWORD dwBufLen = sizeof( AdapterInfo );
    DWORD dwStatus = GetAdaptersInfo( AdapterInfo, &dwBufLen );
    if ( dwStatus != ERROR_SUCCESS )
        return; // no adapters.

    PIP_ADAPTER_INFO pAdapterInfo = AdapterInfo;
    mac1 = (int*) pAdapterInfo;
    if ( pAdapterInfo->Next )
        mac2 = (int*) pAdapterInfo->Next;
#endif

done:
    return 0;

failed:
    return -1;
}

int DSOLOCAL msc_beacon_call_unique_id (unsigned char *digest)
{
    unsigned char local_digest[APR_SHA1_DIGESTSIZE];
    apr_sha1_ctx_t context;
    char *input;
    int i = 0;
    unsigned char *mac_address = NULL;
    char *machine_name = NULL;
    int ret = 0;
    
    mac_address = malloc(sizeof(char)*(6+5+1));
    if (!mac_address)
    {
        ret = -1;
        goto failed_mac_address;
    }
    memset(mac_address, 0, sizeof(char)*(6+5+1));

    if (msc_beacon_call_mac_address(mac_address))
    {
        ret = -1;
        goto failed_set_mac_address;
    }

    machine_name = malloc(sizeof(char)*201);
    if (!machine_name)
    {
        ret = -1;
        goto failed_machine_name;
    }

    if (msc_beacon_call_machine_name(machine_name, 200))
    {
        ret = -1;
        goto failed_set_machine_name;
    }

    input = malloc(sizeof(char)*(strlen(machine_name) +
        strlen(mac_address)+1));
    if (!input)
    {
        goto failed_input;
    }

    snprintf(input, strlen(machine_name)+strlen(mac_address)+1,
        "%s%s\0", machine_name, mac_address);

    apr_sha1_init(&context);
    apr_sha1_update(&context, input, strlen(input));
    apr_sha1_final(local_digest, &context);

    for (i = 0; i < APR_SHA1_DIGESTSIZE; i++)
    {
        sprintf(digest, "%s%02x", digest, local_digest[i]);
    }

    free(input);
failed_input:
failed_set_machine_name:
    free(machine_name);
failed_machine_name:
failed_set_mac_address:
    free(mac_address);
failed_mac_address:
    
    return ret;
}

int msc_beacon_call (void)
{
    char *apr;
    const char *apr_loaded;
    char pcre[7];
    const char *pcre_loaded;
    char *lua;
    char *lua_loaded;
    char *libxml;
    char *libxml_loaded;
    char *modsec;
    const char *apache;
    char *id;
    char *beacon_string;
    int beacon_string_len = 0;
    char *beacon_string_encoded;
    char *beacon_string_ready;
    char *beacon_string_encoded_splitted;
    int ret = 0;

    apr = APR_VERSION_STRING;
    apr_loaded = apr_version_string();

    pcre_loaded = pcre_version();
    snprintf(pcre, 7, "%2d.%2d \0", PCRE_MAJOR, PCRE_MINOR);
#ifdef WITH_LUA
    lua = LUA_VERSION;
#endif
    lua_loaded = 0;

    libxml = LIBXML_DOTTED_VERSION;
    libxml_loaded = 0;

    modsec = MODSEC_VERSION;
    apache = apache_get_server_version();

    id = malloc(sizeof(char)*((2*APR_SHA1_DIGESTSIZE)+1));
    if (!id)
    {
        ret = -1;
        goto failed_id;
    }

    memset(id, '\0', sizeof(char)*((2*APR_SHA1_DIGESTSIZE)+1));
    if (msc_beacon_call_unique_id(id))
    {
        sprintf(id, "unique id failed\0");
    }

    beacon_string_len = (modsec?strlen(modsec):0) + (apache?strlen(apache):0) +
        (apr?strlen(apr):0) + (apr_loaded?strlen(apr_loaded):0) +
        (pcre?strlen(pcre):0) + (pcre_loaded?strlen(pcre_loaded):0) +
        (lua?strlen(lua):0) + (lua_loaded?strlen(lua_loaded):0) +
        (libxml?strlen(libxml):0) + (libxml_loaded?strlen(libxml_loaded):0) +
        (id?strlen(id):0);

    beacon_string = malloc(sizeof(char)*(beacon_string_len+1+10));
    if (!beacon_string)
    {
        goto failed_beacon_string;
    }


    snprintf(beacon_string, beacon_string_len+1+10,
        "%s,%s,%s/%s,%s/%s,%s/%s,%s/%s,%s",
        modsec, apache, apr, apr_loaded, pcre, pcre_loaded, lua, lua_loaded,
        libxml, libxml_loaded, id);

    ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, NULL,
            "ModSecurity: Beacon call: \"%s\"", beacon_string);

    beacon_string_encoded = malloc(sizeof(char)*(strlen(beacon_string)*3));
    if (!beacon_string_encoded)
    {
        goto failed_beacon_string_encoded;
    }
    msc_beacon_call_base32_encode(beacon_string_encoded, beacon_string,
        strlen(beacon_string)*3);

    beacon_string_encoded_splitted = malloc (sizeof(char) *
        ((strlen(beacon_string_encoded)/BEACON_CALL_IN_BETWEEN_DOTS) + 
         strlen(beacon_string_encoded) + 1 + 1));
    if (!beacon_string_encoded_splitted)
    {
        goto failed_beacon_string_encoded_splitted;
    }

    msc_beacon_call_fill_with_dots(beacon_string_encoded_splitted,
        beacon_string_encoded,
        (strlen(beacon_string_encoded)/BEACON_CALL_IN_BETWEEN_DOTS) +
                     strlen(beacon_string_encoded) + 1 + 1,
                     BEACON_CALL_IN_BETWEEN_DOTS);

    beacon_string_ready = malloc (sizeof(char) *
        strlen(beacon_string_encoded_splitted) +
        strlen(MODSECURITY_DNS_BEACON_POSTFIX) + 10 + 1);

    if (!beacon_string_ready)
    {
        goto failed_beacon_string_ready;
    }

    snprintf(beacon_string_ready, strlen(beacon_string_encoded_splitted) +
        strlen(MODSECURITY_DNS_BEACON_POSTFIX) + 12 + 1, "%s.%lu.%s\0",
        beacon_string_encoded_splitted, time(NULL),
        MODSECURITY_DNS_BEACON_POSTFIX);

    ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, NULL,
        "ModSecurity: Beacon call encoded: \"%s\"", beacon_string_ready);

    if (gethostbyname(beacon_string_ready))
    {
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, NULL,
            "ModSecurity: Beacon call successfully submitted. For more " \
            "information visit: http://%s/", beacon_string_ready);
    }
    else
    {
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, NULL,
            "ModSecurity: Beacon call failed.");
    }

    free(beacon_string_ready);
failed_beacon_string_ready:
    free(beacon_string_encoded_splitted);
failed_beacon_string_encoded_splitted:
    free(beacon_string_encoded);
failed_beacon_string_encoded:
    free(beacon_string);
failed_beacon_string:
    free(id);
failed_id:

    return ret;
}

