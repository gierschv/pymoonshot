/**
 * Copyright (c) 2006-2010 Apple Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <Python.h>
#include "kerberosgss.h"

#include "base64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

static void set_gss_error(OM_uint32 err_maj, OM_uint32 err_min);
static void parse_oid(const char *mechanism, gss_OID *oid);

extern PyObject *GssException_class;
extern PyObject *KrbException_class;

char* server_principal_details(const char* service, const char* hostname)
{
    char match[1024];
    int match_len = 0;
    char* result = NULL;
    
    int code;
    krb5_context kcontext;
    krb5_keytab kt = NULL;
    krb5_kt_cursor cursor = NULL;
    krb5_keytab_entry entry;
    char* pname = NULL;
    
    // Generate the principal prefix we want to match
    snprintf(match, 1024, "%s/%s@", service, hostname);
    match_len = strlen(match);
    
    code = krb5_init_context(&kcontext);
    if (code)
    {
        PyErr_SetObject(KrbException_class, Py_BuildValue("((s:i))",
                                                          "Cannot initialize Kerberos5 context", code));
        return NULL;
    }
    
    if ((code = krb5_kt_default(kcontext, &kt)))
    {
        PyErr_SetObject(KrbException_class, Py_BuildValue("((s:i))",
                                                          "Cannot get default keytab", code));
        goto end;
    }
    
    if ((code = krb5_kt_start_seq_get(kcontext, kt, &cursor)))
    {
        PyErr_SetObject(KrbException_class, Py_BuildValue("((s:i))",
                                                          "Cannot get sequence cursor from keytab", code));
        goto end;
    }
    
    while ((code = krb5_kt_next_entry(kcontext, kt, &entry, &cursor)) == 0)
    {
        if ((code = krb5_unparse_name(kcontext, entry.principal, &pname)))
        {
            PyErr_SetObject(KrbException_class, Py_BuildValue("((s:i))",
                                                              "Cannot parse principal name from keytab", code));
            goto end;
        }
        
        if (strncmp(pname, match, match_len) == 0)
        {
            result = malloc(strlen(pname) + 1);
            strcpy(result, pname);
            krb5_free_unparsed_name(kcontext, pname);
            krb5_free_keytab_entry_contents(kcontext, &entry);
            break;
        }
        
        krb5_free_unparsed_name(kcontext, pname);
        krb5_free_keytab_entry_contents(kcontext, &entry);
    }
    
    if (result == NULL)
    {
        PyErr_SetObject(KrbException_class, Py_BuildValue("((s:i))",
                                                          "Principal not found in keytab", -1));
    }
    
end:
    if (cursor)
        krb5_kt_end_seq_get(kcontext, kt, &cursor);
    if (kt)
        krb5_kt_close(kcontext, kt);
    krb5_free_context(kcontext);
    
    return result;
}

int authenticate_gss_client_init(const char* service, long int gss_flags, const char *mech_oid, gss_client_state* state)
{
    OM_uint32 maj_stat;
    OM_uint32 min_stat;
    gss_buffer_desc name_token = GSS_C_EMPTY_BUFFER;
    int ret = AUTH_GSS_COMPLETE;
    
    state->server_name = GSS_C_NO_NAME;
    state->context = GSS_C_NO_CONTEXT;
    state->gss_flags = gss_flags;
    state->username = NULL;
    state->response = NULL;

    if (mech_oid == NULL)
        state->mech_type = GSS_C_NO_OID;
    else
        parse_oid(mech_oid, &state->mech_type);
    
    // Import server name first
    name_token.length = strlen(service);
    name_token.value = (char *)service;
    
    maj_stat = gss_import_name(&min_stat, &name_token, gss_krb5_nt_service_name, &state->server_name);
    
    if (GSS_ERROR(maj_stat))
    {
        set_gss_error(maj_stat, min_stat);
        ret = AUTH_GSS_ERROR;
        goto end;
    }
    
end:
    return ret;
}

int authenticate_gss_client_clean(gss_client_state *state)
{
    OM_uint32 maj_stat;
    OM_uint32 min_stat;
    int ret = AUTH_GSS_COMPLETE;
    
    if (state->context != GSS_C_NO_CONTEXT)
        maj_stat = gss_delete_sec_context(&min_stat, &state->context, GSS_C_NO_BUFFER);
    if (state->server_name != GSS_C_NO_NAME)
        maj_stat = gss_release_name(&min_stat, &state->server_name);
    if (state->username != NULL)
    {
        free(state->username);
        state->username = NULL;
    }
    if (state->response != NULL)
    {
        free(state->response);
        state->response = NULL;
    }
    
    return ret;
}

int authenticate_gss_client_step(gss_client_state* state, const char* challenge)
{
    OM_uint32 maj_stat;
    OM_uint32 min_stat;
    gss_buffer_desc input_token = GSS_C_EMPTY_BUFFER;
    gss_buffer_desc output_token = GSS_C_EMPTY_BUFFER;
    int ret = AUTH_GSS_CONTINUE;
    
    // Always clear out the old response
    if (state->response != NULL)
    {
        free(state->response);
        state->response = NULL;
    }
    
    // If there is a challenge (data from the server) we need to give it to GSS
    if (challenge && *challenge)
    {
        int len;
        input_token.value = base64_decode(challenge, &len);
        input_token.length = len;
    }
    
    // Do GSSAPI step
    maj_stat = gss_init_sec_context(&min_stat,
                                    GSS_C_NO_CREDENTIAL,
                                    &state->context,
                                    state->server_name,
                                    state->mech_type,
                                    (OM_uint32)state->gss_flags,
                                    0,
                                    GSS_C_NO_CHANNEL_BINDINGS,
                                    &input_token,
                                    NULL,
                                    &output_token,
                                    NULL,
                                    NULL);
    
    if ((maj_stat != GSS_S_COMPLETE) && (maj_stat != GSS_S_CONTINUE_NEEDED))
    {
        set_gss_error(maj_stat, min_stat);
        ret = AUTH_GSS_ERROR;
        goto end;
    }
    
    ret = (maj_stat == GSS_S_COMPLETE) ? AUTH_GSS_COMPLETE : AUTH_GSS_CONTINUE;
    // Grab the client response to send back to the server
    if (output_token.length)
    {
        state->response = base64_encode((const unsigned char *)output_token.value, output_token.length);;
        maj_stat = gss_release_buffer(&min_stat, &output_token);
    }
    
    // Try to get the user name if we have completed all GSS operations
    if (ret == AUTH_GSS_COMPLETE)
    {
        gss_name_t gssuser = GSS_C_NO_NAME;
        maj_stat = gss_inquire_context(&min_stat, state->context, &gssuser, NULL, NULL, NULL,  NULL, NULL, NULL);
        if (GSS_ERROR(maj_stat))
        {
            set_gss_error(maj_stat, min_stat);
            ret = AUTH_GSS_ERROR;
            goto end;
        }
        
        gss_buffer_desc name_token;
        name_token.length = 0;
        maj_stat = gss_display_name(&min_stat, gssuser, &name_token, NULL);
        if (GSS_ERROR(maj_stat))
        {
            if (name_token.value)
                gss_release_buffer(&min_stat, &name_token);
            gss_release_name(&min_stat, &gssuser);
            
            set_gss_error(maj_stat, min_stat);
            ret = AUTH_GSS_ERROR;
            goto end;
        }
        else
        {
            state->username = (char *)malloc(name_token.length + 1);
            strncpy(state->username, (char*) name_token.value, name_token.length);
            state->username[name_token.length] = 0;
            gss_release_buffer(&min_stat, &name_token);
            gss_release_name(&min_stat, &gssuser);
        }
    }
end:
    if (output_token.value)
        gss_release_buffer(&min_stat, &output_token);
    if (input_token.value)
        free(input_token.value);
    return ret;
}

int authenticate_gss_client_unwrap(gss_client_state *state, const char *challenge)
{
	OM_uint32 maj_stat;
	OM_uint32 min_stat;
	gss_buffer_desc input_token = GSS_C_EMPTY_BUFFER;
	gss_buffer_desc output_token = GSS_C_EMPTY_BUFFER;
	int ret = AUTH_GSS_CONTINUE;
    
	// Always clear out the old response
	if (state->response != NULL)
	{
		free(state->response);
		state->response = NULL;
	}
    
	// If there is a challenge (data from the server) we need to give it to GSS
	if (challenge && *challenge)
	{
		int len;
		input_token.value = base64_decode(challenge, &len);
		input_token.length = len;
	}
    
	// Do GSSAPI step
	maj_stat = gss_unwrap(&min_stat,
                          state->context,
                          &input_token,
                          &output_token,
                          NULL,
                          NULL);
    
	if (maj_stat != GSS_S_COMPLETE)
	{
		set_gss_error(maj_stat, min_stat);
		ret = AUTH_GSS_ERROR;
		goto end;
	}
	else
		ret = AUTH_GSS_COMPLETE;
    
	// Grab the client response
	if (output_token.length)
	{
		state->response = base64_encode((const unsigned char *)output_token.value, output_token.length);
		maj_stat = gss_release_buffer(&min_stat, &output_token);
	}
end:
	if (output_token.value)
		gss_release_buffer(&min_stat, &output_token);
	if (input_token.value)
		free(input_token.value);
	return ret;
}

int authenticate_gss_client_wrap(gss_client_state* state, const char* challenge, const char* user)
{
	OM_uint32 maj_stat;
	OM_uint32 min_stat;
	gss_buffer_desc input_token = GSS_C_EMPTY_BUFFER;
	gss_buffer_desc output_token = GSS_C_EMPTY_BUFFER;
	int ret = AUTH_GSS_CONTINUE;
	char buf[4096], server_conf_flags;
	unsigned long buf_size;
    
	// Always clear out the old response
	if (state->response != NULL)
	{
		free(state->response);
		state->response = NULL;
	}
    
	if (challenge && *challenge)
	{
		int len;
		input_token.value = base64_decode(challenge, &len);
		input_token.length = len;
	}
    
	if (user) {
		// get bufsize
		server_conf_flags = ((char*) input_token.value)[0];
		((char*) input_token.value)[0] = 0;
		buf_size = ntohl(*((long *) input_token.value));
		free(input_token.value);
#ifdef PRINTFS
		printf("User: %s, %c%c%c\n", user,
               server_conf_flags & GSS_AUTH_P_NONE      ? 'N' : '-',
               server_conf_flags & GSS_AUTH_P_INTEGRITY ? 'I' : '-',
               server_conf_flags & GSS_AUTH_P_PRIVACY   ? 'P' : '-');
		printf("Maximum GSS token size is %ld\n", buf_size);
#endif
        
		// agree to terms (hack!)
		buf_size = htonl(buf_size); // not relevant without integrity/privacy
		memcpy(buf, &buf_size, 4);
		buf[0] = GSS_AUTH_P_NONE;
		// server decides if principal can log in as user
		strncpy(buf + 4, user, sizeof(buf) - 4);
		input_token.value = buf;
		input_token.length = 4 + strlen(user);
	}
    
	// Do GSSAPI wrap
	maj_stat = gss_wrap(&min_stat,
						state->context,
						0,
						GSS_C_QOP_DEFAULT,
						&input_token,
						NULL,
						&output_token);
    
	if (maj_stat != GSS_S_COMPLETE)
	{
		set_gss_error(maj_stat, min_stat);
		ret = AUTH_GSS_ERROR;
		goto end;
	}
	else
		ret = AUTH_GSS_COMPLETE;
	// Grab the client response to send back to the server
	if (output_token.length)
	{
		state->response = base64_encode((const unsigned char *)output_token.value, output_token.length);;
		maj_stat = gss_release_buffer(&min_stat, &output_token);
	}
end:
	if (output_token.value)
		gss_release_buffer(&min_stat, &output_token);
	return ret;
}

int authenticate_gss_server_init(const char *service, const char *desired_mech_oid, gss_server_state *state)
{
    OM_uint32 maj_stat;
    OM_uint32 min_stat;
    gss_buffer_desc name_token = GSS_C_EMPTY_BUFFER;
    int ret = AUTH_GSS_COMPLETE;
    
    state->context = GSS_C_NO_CONTEXT;
    state->server_name = GSS_C_NO_NAME;
    state->client_name = GSS_C_NO_NAME;
    state->server_creds = GSS_C_NO_CREDENTIAL;
    state->client_creds = GSS_C_NO_CREDENTIAL;
    state->username = NULL;
    state->targetname = NULL;
    state->response = NULL;
    state->maj_stat = AUTH_GSS_CONTINUE;
    state->attributes_keys = NULL;
    state->attributes_values = NULL;
    
    // Server name may be empty which means we aren't going to create our own creds
    size_t service_len = strlen(service);
    if (service_len != 0)
    {
        // Import server name first
        name_token.length = strlen(service);
        name_token.value = (char *)service;
        
        maj_stat = gss_import_name(&min_stat, &name_token, GSS_C_NT_HOSTBASED_SERVICE, &state->server_name);
        
        if (GSS_ERROR(maj_stat))
        {
            set_gss_error(maj_stat, min_stat);
            ret = AUTH_GSS_ERROR;
            goto end;
        }
        
        // Get credentials
        // No desired mechanism precised, use GSS_C_NO_OID_SET instead
        if (desired_mech_oid == NULL) {
            maj_stat = gss_acquire_cred(&min_stat, state->server_name, GSS_C_INDEFINITE,
                                        GSS_C_NO_OID_SET, GSS_C_ACCEPT, &state->server_creds, NULL, NULL);
        }
        else {
            gss_OID_set_desc mechs;
            gss_OID mech_oid;
            parse_oid(desired_mech_oid, &mech_oid);

            mechs.count = 1;
            mechs.elements = mech_oid;
            maj_stat = gss_acquire_cred(&min_stat, state->server_name, GSS_C_INDEFINITE,
                                        &mechs, GSS_C_ACCEPT, &state->server_creds, NULL, NULL);

        }
        
        if (GSS_ERROR(maj_stat))
        {
            set_gss_error(maj_stat, min_stat);
            ret = AUTH_GSS_ERROR;
            goto end;
        }
    }
    
end:
    return ret;
}

int authenticate_gss_server_clean(gss_server_state *state)
{
    OM_uint32 maj_stat;
    OM_uint32 min_stat;
    int ret = AUTH_GSS_COMPLETE;
    
    if (state->context != GSS_C_NO_CONTEXT)
        maj_stat = gss_delete_sec_context(&min_stat, &state->context, GSS_C_NO_BUFFER);
    if (state->server_name != GSS_C_NO_NAME)
        maj_stat = gss_release_name(&min_stat, &state->server_name);
    if (state->client_name != GSS_C_NO_NAME)
        maj_stat = gss_release_name(&min_stat, &state->client_name);
    if (state->server_creds != GSS_C_NO_CREDENTIAL)
        maj_stat = gss_release_cred(&min_stat, &state->server_creds);
    if (state->client_creds != GSS_C_NO_CREDENTIAL)
        maj_stat = gss_release_cred(&min_stat, &state->client_creds);
    if (state->username != NULL)
    {
        free(state->username);
        state->username = NULL;
    }
    if (state->targetname != NULL)
    {
        free(state->targetname);
        state->targetname = NULL;
    }
    if (state->response != NULL)
    {
        free(state->response);
        state->response = NULL;
    }
    if (state->attributes_keys != NULL)
    {
        size_t i = 0;
        while (state->attributes_keys[i] != NULL)
            free(state->attributes_keys[i++]);
        free(state->attributes_keys);
        state->attributes_keys = NULL;
    }
    if (state->attributes_values != NULL)
    {
        size_t i = 0;
        while (state->attributes_values[i] != NULL)
            free(state->attributes_values[i++]);
        free(state->attributes_values);
        state->attributes_values = NULL;
    }
    
    return ret;
}

int authenticate_gss_server_step(gss_server_state *state, const char *challenge)
{
    OM_uint32 maj_stat;
    OM_uint32 min_stat;
    gss_buffer_desc input_token = GSS_C_EMPTY_BUFFER;
    gss_buffer_desc output_token = GSS_C_EMPTY_BUFFER;
    int ret = AUTH_GSS_CONTINUE;
    
    // Always clear out the old response
    if (state->response != NULL)
    {
        free(state->response);
        state->response = NULL;
    }
    
    // If there is a challenge (data from the server) we need to give it to GSS
    if (challenge && *challenge)
    {
        int len;
        input_token.value = base64_decode(challenge, &len);
        input_token.length = len;
    }
    else
    {
        PyErr_SetString(KrbException_class, "No challenge parameter in request from client");
        ret = AUTH_GSS_ERROR;
        goto end;
    }
    
    state->maj_stat = gss_accept_sec_context(&min_stat,
                                             &state->context,
                                             state->server_creds,
                                             &input_token,
                                             GSS_C_NO_CHANNEL_BINDINGS,
                                             &state->client_name,
                                             NULL,
                                             &output_token,
                                             NULL,
                                             NULL,
                                             &state->client_creds);
    if (GSS_ERROR(state->maj_stat))
    {
        set_gss_error(state->maj_stat, min_stat);
        ret = AUTH_GSS_ERROR;
        goto end;
    }

    // Grab the server response to send back to the client
    if (output_token.length)
    {
        state->response = base64_encode((const unsigned char *)output_token.value, output_token.length);
        maj_stat = gss_release_buffer(&min_stat, &output_token);
        if (state->maj_stat != GSS_S_COMPLETE) {
            maj_stat = state->maj_stat;
            goto end;
        }
    }
    
    // Get the user name
    maj_stat = gss_display_name(&min_stat, state->client_name, &output_token, NULL);
    if (GSS_ERROR(maj_stat))
    {
        set_gss_error(maj_stat, min_stat);
        ret = AUTH_GSS_ERROR;
        goto end;
    }
    state->username = (char *)malloc(output_token.length + 1);
    strncpy(state->username, (char*) output_token.value, output_token.length);
    state->username[output_token.length] = 0;
    
    // Get the target name if no server creds were supplied
    if (state->server_creds == GSS_C_NO_CREDENTIAL)
    {
        gss_name_t target_name = GSS_C_NO_NAME;
        maj_stat = gss_inquire_context(&min_stat, state->context, NULL, &target_name, NULL, NULL, NULL, NULL, NULL);
        if (GSS_ERROR(maj_stat))
        {
            set_gss_error(maj_stat, min_stat);
            ret = AUTH_GSS_ERROR;
            goto end;
        }
        maj_stat = gss_display_name(&min_stat, target_name, &output_token, NULL);
        if (GSS_ERROR(maj_stat))
        {
            set_gss_error(maj_stat, min_stat);
            ret = AUTH_GSS_ERROR;
            goto end;
        }
        state->targetname = (char *)malloc(output_token.length + 1);
        strncpy(state->targetname, (char*) output_token.value, output_token.length);
        state->targetname[output_token.length] = 0;
    }

    ret = AUTH_GSS_COMPLETE;
    
end:
    if (output_token.length)
        gss_release_buffer(&min_stat, &output_token);
    if (input_token.value)
        free(input_token.value);
    return ret;
}

static char* gss_server_dump_attribute(gss_name_t name, gss_buffer_t attribute)
{
    OM_uint32 maj_stat;
    OM_uint32 min_stat;
    gss_buffer_desc value;
    gss_buffer_desc display_value;

    int authenticated = 0;
    int complete = 0;
    int more = -1;

    char *result = NULL;
    size_t result_pos = 0;

    while (more != 0) {
        value.value = NULL;
        display_value.value = NULL;

        maj_stat = gss_get_name_attribute(&min_stat, name, attribute, &authenticated,
                                          &complete, &value, &display_value,
                                          &more);
        if (GSS_ERROR(maj_stat))
        {
            set_gss_error(maj_stat, min_stat);
            if (result != NULL)
                free(result);
            return NULL;
        }

        result = realloc(result, (result == NULL ? 0 : result_pos) + display_value.length + 1);
        memcpy(result + result_pos, display_value.value, display_value.length);
        result_pos += display_value.length;
        result[result_pos] = '\0';

        gss_release_buffer(&min_stat, &value);
        gss_release_buffer(&min_stat, &display_value);
    }

    return result;
}

int authenticate_gss_server_attributes(gss_server_state *state)
{
    if (state->maj_stat != GSS_S_COMPLETE)
        return AUTH_GSS_ERROR;

    if (state->attributes_keys != NULL)
        return AUTH_GSS_COMPLETE;

    OM_uint32 maj_stat;
    OM_uint32 min_stat;
    gss_OID mech = GSS_C_NO_OID;
    gss_buffer_set_t attrs = GSS_C_NO_BUFFER_SET;

    int name_is_MN;
    size_t i;

    maj_stat = gss_inquire_name(&min_stat, state->client_name, &name_is_MN, &mech, &attrs);
    if (GSS_ERROR(maj_stat))
    {
        set_gss_error(maj_stat, min_stat);
        return AUTH_GSS_ERROR;
    }

    if (attrs != GSS_C_NO_BUFFER_SET)
    {
        state->attributes_keys = malloc((attrs->count + 1) * sizeof(char*));
        state->attributes_values = malloc((attrs->count + 1) * sizeof(char*));
        if (state->attributes_keys == NULL || state->attributes_values == NULL)
            return AUTH_GSS_ERROR;
        for (i = 0; i < attrs->count; i++) {
            state->attributes_keys[i] = malloc(attrs->elements[i].length + 1);
            if (state->attributes_keys[i] != NULL)
            {
                memcpy(state->attributes_keys[i], attrs->elements[i].value, attrs->elements[i].length);
                state->attributes_keys[i][attrs->elements[i].length] = '\0';
            }
            state->attributes_values[i] = gss_server_dump_attribute(state->client_name, &attrs->elements[i]);
        }
        state->attributes_keys[i] = NULL;
        state->attributes_values[i] = NULL;
    }

    gss_release_oid(&min_stat, &mech);
    gss_release_buffer_set(&min_stat, &attrs);
    return AUTH_GSS_COMPLETE;
}

static void set_gss_error(OM_uint32 err_maj, OM_uint32 err_min)
{
    OM_uint32 maj_stat, min_stat;
    OM_uint32 msg_ctx = 0;
    gss_buffer_desc status_string;
    char buf_maj[512];
    char buf_min[512];
    
    do
    {
        maj_stat = gss_display_status (&min_stat,
                                       err_maj,
                                       GSS_C_GSS_CODE,
                                       GSS_C_NO_OID,
                                       &msg_ctx,
                                       &status_string);
        if (GSS_ERROR(maj_stat))
            break;
        strncpy(buf_maj, (char*) status_string.value, sizeof(buf_maj));
        gss_release_buffer(&min_stat, &status_string);
        
        maj_stat = gss_display_status (&min_stat,
                                       err_min,
                                       GSS_C_MECH_CODE,
                                       GSS_C_NULL_OID,
                                       &msg_ctx,
                                       &status_string);
        if (!GSS_ERROR(maj_stat))
        {
            strncpy(buf_min, (char*) status_string.value, sizeof(buf_min));
            gss_release_buffer(&min_stat, &status_string);
        }
    } while (!GSS_ERROR(maj_stat) && msg_ctx != 0);
    
    PyErr_SetObject(GssException_class, Py_BuildValue("((s:i)(s:i))", buf_maj, err_maj, buf_min, err_min));
}

static void parse_oid(const char *mechanism, gss_OID *oid)
{
    char        *mechstr = 0, *cp;
    gss_buffer_desc tok;
    OM_uint32 maj_stat, min_stat;
   
    if (isdigit(mechanism[0])) {
        mechstr = malloc(strlen(mechanism)+5);
        if (!mechstr) {
            printf("Couldn't allocate mechanism scratch!\n");
            return;
        }
        sprintf(mechstr, "{ %s }", mechanism);
        for (cp = mechstr; *cp; cp++)
            if (*cp == '.')
                *cp = ' ';
        tok.value = mechstr;
    } else
        tok.value = (char*) mechanism;
    tok.length = strlen(tok.value);
    maj_stat = gss_str_to_oid(&min_stat, &tok, oid);
    if (maj_stat != GSS_S_COMPLETE) {
        set_gss_error(maj_stat, min_stat);
        return;
    }
    if (mechstr)
        free(mechstr);
}
