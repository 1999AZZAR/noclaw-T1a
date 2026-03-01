/*
 * Telegram channel implementation using minimalist BearSSL + native HTTP.
 */

#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
    char token[128];
    long last_update_id;
} tg_ctx;

static void tg_set_typing(tg_ctx *ctx, long chat_id) {
    char url[512], body[128];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendChatAction", ctx->token);
    snprintf(body, sizeof(body), "{\"chat_id\":%ld,\"action\":\"typing\"}", chat_id);
    const char *hdrs[] = {"Content-Type: application/json"};
    nc_http_response resp;
    if (nc_http_post(url, body, strlen(body), hdrs, 1, &resp)) {
        nc_http_response_free(&resp);
    }
}

static void tg_send_msg(tg_ctx *ctx, long chat_id, const char *text) {
    if (!text || !text[0]) return;
    char url[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", ctx->token);
    
    size_t body_sz = strlen(text) * 2 + 1024;
    char *body = malloc(body_sz);
    if (!body) return;

    int off = 0;
    off += snprintf(body + off, body_sz - (size_t)off, "{\"chat_id\":%ld,\"text\":\"", chat_id);
    
    for (const char *p = text; *p; p++) {
        if (off > (int)body_sz - 32) break;
        if (*p == '"') { body[off++] = '\\'; body[off++] = '"'; }
        else if (*p == '\\') { body[off++] = '\\'; body[off++] = '\\'; }
        else if (*p == '\n') { body[off++] = '\\'; body[off++] = 'n'; }
        else if (*p == '\r') { body[off++] = '\\'; body[off++] = 'r'; }
        else if (*p == '\t') { body[off++] = '\\'; body[off++] = 't'; }
        else if ((unsigned char)*p >= 32) body[off++] = *p;
    }
    
    off += snprintf(body + off, body_sz - (size_t)off, "\",\"parse_mode\":\"Markdown\"}");

    const char *hdrs[] = {"Content-Type: application/json"};
    nc_http_response resp;
    if (nc_http_post(url, body, (size_t)off, hdrs, 1, &resp)) {
        nc_http_response_free(&resp);
    }
    free(body);
}

static void tg_poll(nc_channel *self, nc_agent *agent) {
    tg_ctx *ctx = (tg_ctx *)self->ctx;
    char url[512], body[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getUpdates", ctx->token);
    
    int off = 0;
    off += snprintf(body + off, sizeof(body) - (size_t)off, "{");
    if (ctx->last_update_id > 0) {
        off += snprintf(body + off, sizeof(body) - (size_t)off, "\"offset\":%ld,", ctx->last_update_id + 1);
    }
    off += snprintf(body + off, sizeof(body) - (size_t)off, "\"timeout\":30}");

    const char *hdrs[] = {"Content-Type: application/json"};
    nc_http_response resp;
    
    nc_log(NC_LOG_INFO, "Polling Telegram with token %s...", ctx->token);
    if (!nc_http_post(url, body, strlen(body), hdrs, 1, &resp)) {
        nc_log(NC_LOG_ERROR, "TG poll failed (network)");
        sleep(5);
        return;
    }

    if (resp.status != 200) {
        nc_log(NC_LOG_ERROR, "TG poll failed (HTTP %d): %.*s", resp.status, (int)resp.body_len, resp.body);
        nc_http_response_free(&resp);
        sleep(5);
        return;
    }

    nc_arena scratch;
    nc_arena_init(&scratch, resp.body_len * 2 + 8192);
    nc_json *root = nc_json_parse(&scratch, resp.body, resp.body_len);
    if (!root) {
        nc_arena_free(&scratch);
        nc_http_response_free(&resp);
        return;
    }

    nc_json *ok = nc_json_get(root, "ok");
    nc_json *res = nc_json_get(root, "result");
    if (ok && ok->boolean && res && res->type == NC_JSON_ARRAY) {
        for (int i = 0; i < res->array.count; i++) {
            nc_json *upd = &res->array.items[i];
            long uid = (long)nc_json_num(nc_json_get(upd, "update_id"), 0);
            if (uid > ctx->last_update_id) ctx->last_update_id = uid;

            nc_json *msg = nc_json_get(upd, "message");
            if (!msg) continue;

            nc_json *chat = nc_json_get(msg, "chat");
            long chat_id = (long)nc_json_num(nc_json_get(chat, "id"), 0);
            nc_str text = nc_json_str(nc_json_get(msg, "text"), "");

            if (text.len > 0) {
                char *cmd = malloc(text.len + 1);
                memcpy(cmd, text.ptr, text.len);
                cmd[text.len] = '\0';

                nc_log(NC_LOG_INFO, "TG: [%ld] %s", chat_id, cmd);
                
                if (nc_commands_execute(agent, cmd, chat_id, self)) {
                    free(cmd);
                    continue;
                }

                tg_set_typing(ctx, chat_id);
                const char *reply = nc_agent_chat(agent, cmd);
                tg_send_msg(ctx, chat_id, reply);
                free(cmd);
            }
        }
    }

    nc_arena_free(&scratch);
    nc_http_response_free(&resp);
}

static bool tg_send(nc_channel *self, const char *to, const char *text) {
    tg_ctx *ctx = (tg_ctx *)self->ctx;
    if (!to) return false;
    long chat_id = atol(to);
    tg_send_msg(ctx, chat_id, text);
    return true;
}

nc_channel nc_channel_telegram(const char *token) {
    tg_ctx *ctx = calloc(1, sizeof(tg_ctx));
    nc_strlcpy(ctx->token, token, sizeof(ctx->token));
    return (nc_channel){
        .name = "telegram",
        .ctx = ctx,
        .poll = tg_poll,
        .send = tg_send,
        .free = NULL
    };
}
