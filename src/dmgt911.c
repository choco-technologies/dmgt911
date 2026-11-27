#define DMOD_ENABLE_REGISTRATION    ON
#include "dmod.h"
#include "dmgt911.h"
#include "dmdrvi.h"
#include "dmini.h"
#include "dmosi.h"
#include "dmgpio_types.h"
#include "dmi2c_types.h"
#include <errno.h>
#include <string.h>

/*
 * dmgt911: the touch panel with a GT911 (Goodix) controller on I2C, as on the Elecrow CrowPanel 7.0" (DIS08070H).
 *
 * The node follows the generic input interface (DMDRVI_IOCTL_INPUT_*) of dmdrvi: read() / GET_STATE give the contacts in screen
 * coordinates, WAIT_EVENT sleeps until they change. The chip is polled: through read() / GET_STATE when a client asks, and every
 * poll_ms while a client waits - nothing runs in the background.
 *
 * The wiring is done by dmdevfs: the sections of the touch panel, of its I2C bus (dmi2c) and of the reset line are in one
 * friends_group, and the roles of the friends (friend_role=...) tell this driver which node is which:
 *
 *   i2c        the dmi2c node the controller is on (required)
 *   reset      the reset line of the chip, low = reset (when the configuration says reset=yes); a dmgpio pin or a pin of an
 *              expander (dmpca9557): any node that takes dmgpio_ioctl_cmd_set_pins_state
 *
 * The interrupt line of the chip is not used (the driver polls). The address of the chip depends on the level of that line when the
 * reset is released: it is 0x5D when the line is low and 0x14 when it is high. The driver tries the configured address first and the
 * other one when the chip does not answer, and keeps the one that does.
 */

/* Magic set to "G911" */
#define DMGT911_CONTEXT_MAGIC       0x47393131

#define REG_PRODUCT_ID              0x8140U     /* 4 bytes, ASCII: "911" and a zero */
#define REG_RES_X                   0x8048U     /* 16 bits, little-endian (the configuration of the chip) */
#define REG_RES_Y                   0x804AU
#define REG_STATUS                  0x814EU     /* bit 7: a new report, bits 0..3: the number of contacts; written with 0 once read */

#define RAW_MAX                     4095U
#define DEFAULT_ADDRESS             DMGT911_ADDRESS_LOW
#define STATUS_READY                0x80U
#define STATUS_COUNT_MASK           0x0FU
#define CONTACT_SIZE                8U

typedef struct
{
    char   *path;                   /**< Node of the friend (NULL: not there) */
    void   *file;                   /**< Kept open once used */
} node_t;

typedef struct
{
    bool     used;
    uint8_t  id;
    uint8_t  pressure;
    uint16_t x, y;                  /**< Screen coordinates of the last report */
    uint64_t seen_ms;               /**< When the chip reported it last */
} tracked_t;

struct dmdrvi_context
{
    uint32_t              magic;
    dmgt911_config_t      config;
    uint8_t               instance;
    dmosi_mutex_t         lock;
    node_t                i2c;
    node_t                reset;
    bool                  reset_wired;      /**< The configuration says there is a reset line to wait for */
    bool                  started;          /**< The chip answered and was checked */
    tracked_t             tracked[DMGT911_MAX_CONTACTS];
    dmdrvi_input_state_t  current;          /**< The last state computed */
    dmdrvi_input_state_t  handed;           /**< The last state a client got */
    bool                  unseen;           /**< current is not what the client has */
};

static bool is_valid_context(dmdrvi_context_t context)
{
    return context != NULL && context->magic == DMGT911_CONTEXT_MAGIC;
}

/* ---- API ---- */

dmod_dmgt911_api_declaration(1.0, bool, _validate_config, ( const dmgt911_config_t *config ))
{
    if (config == NULL || config->width == 0 || config->height == 0)
        return false;
    if (config->raw_width > RAW_MAX + 1U || config->raw_height > RAW_MAX + 1U)
        return false;
    if (config->address != DMGT911_ADDRESS_LOW && config->address != DMGT911_ADDRESS_HIGH)
        return false;
    return config->max_contacts >= 1 && config->max_contacts <= DMGT911_MAX_CONTACTS && config->poll_ms >= 1;
}

dmod_dmgt911_api_declaration(1.0, uint8_t, _parse_report, ( const uint8_t *report, size_t size, dmgt911_raw_contact_t *contacts, uint8_t max_contacts, bool *ready ))
{
    if (ready != NULL)
        *ready = false;
    if (report == NULL || contacts == NULL || size < DMGT911_REPORT_SIZE)
        return 0;
    if ((report[0] & STATUS_READY) == 0U)
        return 0;
    if (ready != NULL)
        *ready = true;
    uint8_t count = report[0] & STATUS_COUNT_MASK;
    if (count > DMGT911_MAX_CONTACTS)
        count = DMGT911_MAX_CONTACTS;
    if (count > max_contacts)
        count = max_contacts;
    for (uint8_t i = 0; i < count; i++)
    {
        /* id, x low, x high, y low, y high, size low, size high, one reserved byte */
        const uint8_t *c = report + 1U + CONTACT_SIZE * i;
        uint16_t area = (uint16_t)(c[5] | (c[6] << 8));
        contacts[i].id = c[0];
        contacts[i].x = (uint16_t)(c[1] | (c[2] << 8));
        contacts[i].y = (uint16_t)(c[3] | (c[4] << 8));
        contacts[i].pressure = area > 255U ? 255U : (uint8_t)area;
    }
    return count;
}

dmod_dmgt911_api_declaration(1.0, void, _map_point, ( const dmgt911_config_t *config, uint16_t raw_x, uint16_t raw_y, uint16_t *x, uint16_t *y ))
{
    uint32_t rw = config->raw_width, rh = config->raw_height;
    uint32_t px = raw_x, py = raw_y;

    if (config->swap_xy)
    {
        uint32_t t = px; px = py; py = t;
        t = rw; rw = rh; rh = t;
    }
    if (px >= rw)
        px = rw - 1U;
    if (py >= rh)
        py = rh - 1U;
    if (config->mirror_x)
        px = rw - 1U - px;
    if (config->mirror_y)
        py = rh - 1U - py;
    /* Raw 0..rw-1 over 0..width-1 */
    uint32_t sx = rw > 1U ? (px * (uint32_t)(config->width - 1U) + (rw - 1U) / 2U) / (rw - 1U) : 0U;
    uint32_t sy = rh > 1U ? (py * (uint32_t)(config->height - 1U) + (rh - 1U) / 2U) / (rh - 1U) : 0U;
    *x = (uint16_t)sx;
    *y = (uint16_t)sy;
}

/* ---- Configuration ---- */

static bool string_to_switch(const char *text, bool fallback)
{
    if (text == NULL)
        return fallback;
    if (strcmp(text, "on") == 0 || strcmp(text, "yes") == 0 || strcmp(text, "true") == 0 || strcmp(text, "1") == 0)
        return true;
    if (strcmp(text, "off") == 0 || strcmp(text, "no") == 0 || strcmp(text, "false") == 0 || strcmp(text, "0") == 0)
        return false;
    return fallback;
}

static int read_config(dmdrvi_context_t context, dmini_context_t ini)
{
    dmgt911_config_t *c = &context->config;

    context->instance = (uint8_t)dmini_get_int(ini, NULL, "instance", 0);
    c->width        = (uint16_t)dmini_get_int(ini, NULL, "width", 800);
    c->height       = (uint16_t)dmini_get_int(ini, NULL, "height", 480);
    c->raw_width    = (uint16_t)dmini_get_int(ini, NULL, "raw_width", 0);
    c->raw_height   = (uint16_t)dmini_get_int(ini, NULL, "raw_height", 0);
    c->address      = (uint8_t)dmini_get_int(ini, NULL, "address", DEFAULT_ADDRESS);
    c->max_contacts = (uint8_t)dmini_get_int(ini, NULL, "max_contacts", DMGT911_MAX_CONTACTS);
    c->swap_xy      = string_to_switch(dmini_get_string(ini, NULL, "swap_xy", NULL), false);
    c->mirror_x     = string_to_switch(dmini_get_string(ini, NULL, "mirror_x", NULL), false);
    c->mirror_y     = string_to_switch(dmini_get_string(ini, NULL, "mirror_y", NULL), false);
    c->poll_ms      = (uint16_t)dmini_get_int(ini, NULL, "poll_ms", 10);
    c->release_ms   = (uint16_t)dmini_get_int(ini, NULL, "release_ms", 300);
    context->reset_wired = string_to_switch(dmini_get_string(ini, NULL, "reset", NULL), false);

    if (!dmgt911_validate_config(c))
    {
        DMOD_LOG_ERROR("Invalid touch configuration (screen %ux%u, address 0x%02X, %u contacts)\n",
                       c->width, c->height, c->address, (unsigned)c->max_contacts);
        return -EINVAL;
    }
    return 0;
}

/* ---- The nodes of the friends ---- */

static void node_close(node_t *node)
{
    if (node->file != NULL)
        Dmod_FileClose(node->file);
    node->file = NULL;
}

static void node_forget(node_t *node)
{
    node_close(node);
    Dmod_Free(node->path);
    node->path = NULL;
}

static void *node_file(node_t *node)
{
    if (node->file == NULL && node->path != NULL)
        node->file = Dmod_FileOpen(node->path, "r+");
    return node->file;
}

static int gpio_set(node_t *node, bool high)
{
    dmgpio_pins_state_t state = high ? dmgpio_pins_state_all_high : dmgpio_pins_state_all_low;
    void *file = node_file(node);
    if (file == NULL)
        return -ENODEV;
    int ret = Dmod_Ioctl(file, dmgpio_ioctl_cmd_set_pins_state, &state);
    if (ret != 0)
        node_close(node);
    return ret;
}

/* ---- The controller: 16-bit register addresses, high byte first ---- */

static int i2c_transfer(dmdrvi_context_t context, dmi2c_message_t *messages, size_t count)
{
    void *file = node_file(&context->i2c);
    dmi2c_transfer_t transfer = { messages, count };
    if (file == NULL)
        return -ENODEV;
    return Dmod_Ioctl(file, dmi2c_ioctl_cmd_transfer, &transfer);
}

static int reg_read_at(dmdrvi_context_t context, uint8_t address, uint16_t reg, uint8_t *data, size_t size)
{
    uint8_t where[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    dmi2c_message_t messages[2] = {
        { .address = address, .read = false, .data = where, .size = sizeof(where) },
        { .address = address, .read = true,  .data = data,  .size = size },
    };
    return i2c_transfer(context, messages, 2);
}

static int reg_read(dmdrvi_context_t context, uint16_t reg, uint8_t *data, size_t size)
{
    return reg_read_at(context, context->config.address, reg, data, size);
}

/* A write of one byte (the status is cleared with it) */
static int reg_write_byte(dmdrvi_context_t context, uint16_t reg, uint8_t value)
{
    uint8_t buffer[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, value };
    dmi2c_message_t message = { .address = context->config.address, .read = false, .data = buffer, .size = sizeof(buffer) };
    return i2c_transfer(context, &message, 1);
}

static void tracked_clear(dmdrvi_context_t context)
{
    memset(context->tracked, 0, sizeof(context->tracked));
    memset(&context->current, 0, sizeof(context->current));
    memset(&context->handed, 0, sizeof(context->handed));
    context->unseen = false;
}

/* Does the chip answer at the address with its identifier ("911") */
static bool chip_identify(dmdrvi_context_t context, uint8_t address)
{
    uint8_t id[4] = { 0 };
    if (reg_read_at(context, address, REG_PRODUCT_ID, id, sizeof(id)) != 0)
        return false;
    return id[0] == '9' && id[1] == '1' && id[2] == '1';
}

/* Reset the chip (when there is a line for it), find it, read what it says about itself and clear its status */
static int chip_start(dmdrvi_context_t context)
{
    dmgt911_config_t *c = &context->config;
    uint8_t info[2];

    if (context->reset.path != NULL)
    {
        (void)gpio_set(&context->reset, false);
        dmosi_thread_sleep(20);
        (void)gpio_set(&context->reset, true);
        dmosi_thread_sleep(100);                /* the chip takes its configuration after the reset */
    }
    if (!chip_identify(context, c->address))
    {
        uint8_t other = c->address == DMGT911_ADDRESS_LOW ? DMGT911_ADDRESS_HIGH : DMGT911_ADDRESS_LOW;
        if (!chip_identify(context, other))
        {
            DMOD_LOG_ERROR("GT911: no answer at 0x%02X and 0x%02X\n", (unsigned)c->address, (unsigned)other);
            return -ENODEV;
        }
        DMOD_LOG_INFO("GT911: the chip is at 0x%02X, not at 0x%02X\n", (unsigned)other, (unsigned)c->address);
        c->address = other;
    }
    uint16_t res_x = 0, res_y = 0;
    if (reg_read(context, REG_RES_X, info, 2) == 0)
        res_x = (uint16_t)(info[0] | (info[1] << 8));
    if (reg_read(context, REG_RES_Y, info, 2) == 0)
        res_y = (uint16_t)(info[0] | (info[1] << 8));
    (void)reg_write_byte(context, REG_STATUS, 0);   /* what it had to say before is not news */
    DMOD_LOG_INFO("GT911 at 0x%02X: resolution %ux%u\n", (unsigned)c->address, res_x, res_y);
    /* The range of the chip: the configured one, else the one it reports (the screen's when it reports nothing sensible) */
    if (c->raw_width == 0)
        c->raw_width = (res_x != 0 && res_x <= RAW_MAX + 1U) ? res_x : c->width;
    if (c->raw_height == 0)
        c->raw_height = (res_y != 0 && res_y <= RAW_MAX + 1U) ? res_y : c->height;
    return 0;
}

static void try_start(dmdrvi_context_t context)
{
    if (context->started || context->i2c.path == NULL || (context->reset_wired && context->reset.path == NULL))
        return;
    dmosi_mutex_lock(context->lock);
    if (!context->started && chip_start(context) == 0)
    {
        context->started = true;
        tracked_clear(context);
        DMOD_LOG_INFO("GT911 %ux%u started (range %ux%u)\n", context->config.width, context->config.height,
                      context->config.raw_width, context->config.raw_height);
    }
    dmosi_mutex_unlock(context->lock);
}

/* ---- The state of the contacts ---- */

static int tracked_find(dmdrvi_context_t context, uint8_t id)
{
    for (int i = 0; i < DMGT911_MAX_CONTACTS; i++)
    {
        if (context->tracked[i].used && context->tracked[i].id == id)
            return i;
    }
    return -1;
}

static int tracked_free(dmdrvi_context_t context)
{
    for (int i = 0; i < DMGT911_MAX_CONTACTS; i++)
    {
        if (!context->tracked[i].used)
            return i;
    }
    return -1;
}

static void state_add(dmdrvi_input_state_t *state, const tracked_t *t, dmdrvi_input_contact_event_t event)
{
    dmdrvi_input_contact_t *c = &state->contacts[state->contact_count++];
    c->x = t->x;
    c->y = t->y;
    c->id = t->id;
    c->event = (uint8_t)event;
    c->pressure = t->pressure;
    c->size = 0;
}

/* Ask the chip what is touched and compute the new state (the lock is held); a state that was not handed yet is kept as it is */
static int poll_once(dmdrvi_context_t context)
{
    uint8_t report[DMGT911_REPORT_SIZE];
    dmgt911_raw_contact_t raw[DMGT911_MAX_CONTACTS];
    dmdrvi_input_state_t next;
    bool present[DMGT911_MAX_CONTACTS] = { false };
    bool ready = false;

    if (!context->started)
        return -ENODEV;
    if (context->unseen)
        return 0;
    int ret = reg_read(context, REG_STATUS, report, sizeof(report));
    if (ret != 0)
        return ret;
    uint8_t count = dmgt911_parse_report(report, sizeof(report), raw, context->config.max_contacts, &ready);
    if ((report[0] & STATUS_READY) != 0U)
        (void)reg_write_byte(context, REG_STATUS, 0);   /* ready for the next report */
    uint64_t now = Dmod_GetUptime();

    memset(&next, 0, sizeof(next));
    for (uint8_t i = 0; ready && i < count; i++)
    {
        int slot = tracked_find(context, raw[i].id);
        dmdrvi_input_contact_event_t event = slot >= 0 ? DMDRVI_INPUT_CONTACT_MOVE : DMDRVI_INPUT_CONTACT_DOWN;
        if (slot < 0)
            slot = tracked_free(context);
        if (slot < 0)
            continue;
        tracked_t *t = &context->tracked[slot];
        t->used = true;
        t->id = raw[i].id;
        t->pressure = raw[i].pressure;
        t->seen_ms = now;
        dmgt911_map_point(&context->config, raw[i].x, raw[i].y, &t->x, &t->y);
        present[slot] = true;
        state_add(&next, t, event);
    }
    for (int i = 0; i < DMGT911_MAX_CONTACTS; i++)
    {
        tracked_t *t = &context->tracked[i];
        if (!t->used || present[i])
            continue;
        /* A report without it lifts it; with no report at all it stays where it was, for release_ms */
        if (ready || now - t->seen_ms >= context->config.release_ms)
        {
            state_add(&next, t, DMDRVI_INPUT_CONTACT_UP);
            t->used = false;
        }
        else
        {
            state_add(&next, t, DMDRVI_INPUT_CONTACT_MOVE);
        }
    }
    if (!dmdrvi_input_state_equal(&next, &context->handed))
        context->unseen = true;
    context->current = next;
    return 0;
}

/* The state for a client: polled when nothing is waiting to be handed out */
static int get_state(dmdrvi_context_t context, dmdrvi_input_state_t *out)
{
    dmosi_mutex_lock(context->lock);
    int ret = poll_once(context);
    if (ret == 0)
    {
        *out = context->current;
        context->handed = context->current;
        context->unseen = false;
    }
    dmosi_mutex_unlock(context->lock);
    return ret;
}

/* Sleep until the contacts change, for at most *timeout_ms (NULL: until they do) */
static int wait_event(dmdrvi_context_t context, const uint32_t *timeout_ms)
{
    uint64_t start = Dmod_GetUptime();
    bool changed = false;
    bool expired = false;
    int ret = 0;

    while (ret == 0 && !changed && !expired)
    {
        dmosi_mutex_lock(context->lock);
        ret = poll_once(context);
        changed = context->unseen;
        dmosi_mutex_unlock(context->lock);
        if (ret == 0 && !changed)
        {
            expired = timeout_ms != NULL && Dmod_GetUptime() - start >= *timeout_ms;
            if (!expired)
                dmosi_thread_sleep(context->config.poll_ms);
        }
    }
    if (ret != 0)
        return ret;
    return changed ? 0 : -ETIMEDOUT;
}

/* ---- DMOD lifecycle ---- */

int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;
    DMOD_LOG_INFO("DMGT911 module initialized\n");
    return 0;
}

int dmod_deinit(void)
{
    DMOD_LOG_INFO("DMGT911 module deinitialized\n");
    return 0;
}

/* ---- DMDRVI interface ---- */

dmod_dmdrvi_dif_api_declaration(2.0, dmgt911, dmdrvi_context_t, _create, ( dmini_context_t config, dmdrvi_dev_num_t* dev_num ))
{
    if (config == NULL || dev_num == NULL)
        return NULL;
    dmdrvi_context_t context = Dmod_Malloc(sizeof(struct dmdrvi_context));
    if (context == NULL)
        return NULL;
    memset(context, 0, sizeof(*context));
    context->magic = DMGT911_CONTEXT_MAGIC;

    if (read_config(context, config) != 0)
        goto fail;
    context->lock = dmosi_mutex_create(false);
    if (context->lock == NULL)
    {
        DMOD_LOG_ERROR("GT911: out of memory\n");
        goto fail;
    }
    dev_num->flags = DMDRVI_NUM_MAJOR;
    dev_num->major = (dmdrvi_dev_id_t)context->instance;
    return context;

fail:
    context->magic = 0;
    Dmod_Free(context);
    return NULL;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmgt911, void, _free, ( dmdrvi_context_t context ))
{
    if (!is_valid_context(context))
        return;
    /* The nodes of the friends are not touched: dmdevfs tears them down before this call, in reverse order of the sections */
    Dmod_Free(context->i2c.path);
    Dmod_Free(context->reset.path);
    dmosi_mutex_destroy(context->lock);
    context->magic = 0;
    Dmod_Free(context);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmgt911, void, _friend_changed, ( dmdrvi_context_t context, const dmdrvi_friend_info_t* info ))
{
    if (!is_valid_context(context) || info == NULL || info->friend_role == NULL)
        return;
    node_t *node = strcmp(info->friend_role, "i2c") == 0 ? &context->i2c
                 : strcmp(info->friend_role, "reset") == 0 ? &context->reset : NULL;
    if (node == NULL)
        return;
    node_forget(node);
    if (node == &context->i2c)
        context->started = false;                       /* a new bus: the chip is started again */
    if (info->state != dmdrvi_dev_state_ready || info->node_path == NULL)
        return;
    node->path = Dmod_StrDup(info->node_path);
    if (node->path == NULL)
    {
        DMOD_LOG_ERROR("GT911: out of memory for the path of a friend\n");
        return;
    }
    try_start(context);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmgt911, void*, _open, ( dmdrvi_context_t context, int flags, const dmdrvi_dev_num_t* dev_num ))
{
    return is_valid_context(context) ? context : NULL;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmgt911, void, _close, ( dmdrvi_context_t context, void* handle ))
{
}

/* read(): the state of the contacts, one dmdrvi_input_state_t */
dmod_dmdrvi_dif_api_declaration(2.0, dmgt911, dmdrvi_ssize_t, _read, ( dmdrvi_context_t context, void* handle, void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    if (!is_valid_context(context) || buffer == NULL || size < sizeof(dmdrvi_input_state_t))
        return -EINVAL;
    dmdrvi_input_state_t *state = buffer;
    int ret = get_state(context, state);
    return ret != 0 ? ret : (dmdrvi_ssize_t)sizeof(dmdrvi_input_state_t);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmgt911, dmdrvi_ssize_t, _write, ( dmdrvi_context_t context, void* handle, const void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    return -ENOTSUP;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmgt911, int, _ioctl, ( dmdrvi_context_t context, void* handle, int command, void* arg ))
{
    if (!is_valid_context(context))
        return -EINVAL;

    switch (command)
    {
        case DMDRVI_IOCTL_INPUT_GET_INFO:
        {
            if (arg == NULL)
                return -EINVAL;
            dmdrvi_input_info_t *info = arg;
            memset(info, 0, sizeof(*info));
            memcpy(info->name, "GT911", 6);
            info->type = DMDRVI_INPUT_TYPE_TOUCHSCREEN;
            info->capabilities = DMDRVI_INPUT_CAP_CONTACTS | DMDRVI_INPUT_CAP_PRESSURE;
            info->width = context->config.width;
            info->height = context->config.height;
            info->max_contacts = context->config.max_contacts;
            return 0;
        }
        case DMDRVI_IOCTL_INPUT_GET_STATE:
        {
            if (arg == NULL)
                return -EINVAL;
            dmdrvi_input_state_t *state = arg;
            return get_state(context, state);
        }
        case DMDRVI_IOCTL_INPUT_WAIT_EVENT:
        {
            const uint32_t *timeout_ms = arg;
            return wait_event(context, timeout_ms);
        }
        case dmgt911_ioctl_cmd_get_config:
        {
            if (arg == NULL)
                return -EINVAL;
            dmgt911_config_t *config = arg;
            *config = context->config;
            return 0;
        }
        default:
            return -ENOTTY;
    }
}

dmod_dmdrvi_dif_api_declaration(2.0, dmgt911, int, _flush, ( dmdrvi_context_t context, void* handle ))
{
    return is_valid_context(context) ? 0 : -EINVAL;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmgt911, int, _stat, ( dmdrvi_context_t context, const char* path, dmdrvi_stat_t* stat ))
{
    if (!is_valid_context(context) || stat == NULL)
        return -EINVAL;
    stat->size = (dmdrvi_size_t)sizeof(dmdrvi_input_state_t);
    stat->mode = 0444;
    return 0;
}
