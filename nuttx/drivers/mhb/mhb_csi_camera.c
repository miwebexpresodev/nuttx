/*
 * Copyright (c) 2016 Motorola Mobility, LLC.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define DEBUG

#include <errno.h>
#include <debug.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nuttx/config.h>
#include <nuttx/device_cam_ext.h>
#include <nuttx/i2c.h>
#include <arch/board/mods.h>
#include <nuttx/math.h>
#include <nuttx/time.h>
#include <nuttx/device.h>
#include <nuttx/device_slave_pwrctrl.h>
#include <nuttx/mhb/device_mhb.h>
#include <nuttx/mhb/mhb_protocol.h>
#include <nuttx/mhb/mhb_csi_camera.h>
#include "mhb_csi_camera_sm.h"

#include "greybus/v4l2_camera_ext_ctrls.h"
#include "camera_ext.h"

/*
 * This is the reference greybus camera extension driver
 * for MHB (Motorola Mods Hi-speed Bus Archirecture)
 *
 */
#define I2C_RETRIES              5
#define I2C_RETRY_DELAY_US       10000
#define MHB_CDSI_CAM_INSTANCE    0
#define MHB_CDSI_OP_TIMEOUT_NS   2000000000LL
#define CTRL_SET_RETRY_DELAY_US  200000

#define MHB_CAM_PWRCTL_WAIT_MASK 0xFE00
#define MHB_CAM_CDSI_WAIT_MASK   0xFC00
#define MHB_CAM_WAIT_EV_NONE     0x0000

struct mhb_camera_s
{
    struct device *mhb_device;
    struct device *slave_pwr_ctrl;
    struct i2c_dev_s* cam_i2c;
    uint8_t apbe_state;
    uint8_t soc_enabled;
    uint8_t cdsi_instance;
    pthread_mutex_t mutex;
    pthread_cond_t slave_cond;
    pthread_cond_t cdsi_cond;
    uint16_t mhb_wait_event;
};

pthread_mutex_t i2c_mutex;
static struct mhb_camera_s s_mhb_camera;

extern struct camera_ext_format_db mhb_camera_format_db;
extern struct mhb_cdsi_config mhb_camera_csi_config;
extern struct camera_ext_ctrl_db mhb_camera_ctrl_db;

/* Cam I2C Ops */
int mhb_camera_i2c_read_rs(uint16_t i2c_addr,
                    uint8_t *addr, int addr_len,
                    uint8_t *data, int data_len)
{
    int ret = 0;
    struct i2c_msg_s msg[2];
    msg[0].addr   = i2c_addr;
    msg[0].flags  = 0;
    msg[0].buffer = addr;
    msg[0].length = addr_len;

    msg[1].addr   = i2c_addr;
    msg[1].flags  = I2C_M_READ;
    msg[1].buffer = data;
    msg[1].length = data_len;

    pthread_mutex_lock(&i2c_mutex);
    ret = I2C_TRANSFER(s_mhb_camera.cam_i2c, msg, 2);
    pthread_mutex_unlock(&i2c_mutex);
    return ret;

}

int mhb_camera_i2c_read(uint16_t i2c_addr,
                    uint8_t *addr, int addr_len,
                    uint8_t *data, int data_len)
{
    struct i2c_msg_s msg[2];
    int ret = 0;

    msg[0].addr   = i2c_addr;
    msg[0].flags  = 0;
    msg[0].buffer = addr;
    msg[0].length = addr_len;

    msg[1].addr   = i2c_addr;
    msg[1].flags  = I2C_M_READ;
    msg[1].buffer = data;
    msg[1].length = data_len;

    pthread_mutex_lock(&i2c_mutex);
    while (I2C_TRANSFER(s_mhb_camera.cam_i2c, &msg[1], 1) != 0)
    {
        usleep(I2C_RETRY_DELAY_US);
        if (++ret == I2C_RETRIES) break;
    }
    if (ret) {
        CAM_ERR("%s I2C read retried %d of %d\n",
                (ret == I2C_RETRIES)?"FAIL":"INFO", ret, I2C_RETRIES);
        if (ret == I2C_RETRIES) {
            pthread_mutex_unlock(&i2c_mutex);
            return ret;
        }
    }
    ret = I2C_TRANSFER(s_mhb_camera.cam_i2c, &msg[0], 1);
    pthread_mutex_unlock(&i2c_mutex);

    return ret;
}


int mhb_camera_i2c_write(uint16_t i2c_addr, uint8_t *addr, int addr_len)
{
    struct i2c_msg_s msg;
    int ret = 0;

    msg.addr   = i2c_addr;
    msg.flags  = 0;
    msg.buffer = addr;
    msg.length = addr_len;

    pthread_mutex_lock(&i2c_mutex);
    while (I2C_TRANSFER(s_mhb_camera.cam_i2c, &msg, 1) != 0)
    {
        usleep(I2C_RETRY_DELAY_US);
        if (++ret == I2C_RETRIES) break;
    }
    if (ret) {
        CAM_ERR("%s I2C write retries %d of %d\n",
                (ret == I2C_RETRIES)?"FAIL":"INFO", ret, I2C_RETRIES);
    }
    pthread_mutex_unlock(&i2c_mutex);

    return -(ret == I2C_RETRIES);
}

/* 1 bytes value register read */
int mhb_camera_i2c_read_reg1(uint16_t i2c_addr, uint16_t regaddr,
                                        uint8_t *value)
{
    uint8_t addr[2];
    uint8_t data[1];

    addr[0] = regaddr & 0xFF;
    addr[1] = (regaddr >> 8) & 0xFF;

    memset(data, 0, sizeof(data));

    if (mhb_camera_i2c_read(i2c_addr, addr, sizeof(addr), data, sizeof(data)) == 0) {
        *value = data[0];
        CTRL_DBG("read: %02x 0x%04x -> 0x%02x\n", i2c_addr, regaddr, *value);
    } else {
        CAM_ERR("Failed i2c read %02x 0x%04x\n", i2c_addr, regaddr);
        return -1;
    }

    return 0;
}

/* 2 bytes value register read */
int mhb_camera_i2c_read_reg2(uint16_t i2c_addr, uint16_t regaddr,
                                        uint16_t *value)
{
    uint8_t addr[2];
    uint8_t data[2];

    addr[0] = regaddr & 0xFF;
    addr[1] = (regaddr >> 8) & 0xFF;

    memset(data, 0, sizeof(data));

    if (mhb_camera_i2c_read(i2c_addr, addr, sizeof(addr), data, sizeof(data)) == 0) {
        *value = (data[1] << 8) + data[0];
        CTRL_DBG("read: %02x 0x%04x -> 0x%04x\n", i2c_addr, regaddr, *value);
    } else {
        CAM_ERR("Failed i2c read %02x 0x%04x\n", i2c_addr, regaddr);
        return -1;
    }

    return 0;
}

/* 4 bytes value register read */
int mhb_camera_i2c_read_reg4(uint16_t i2c_addr, uint16_t regaddr,
                                 uint32_t *value)
{
    uint8_t addr[2];
    uint8_t data[4];

    addr[0] = regaddr & 0xFF;
    addr[1] = (regaddr >> 8) & 0xFF;

    memset(data, 0, sizeof(data));

    if (mhb_camera_i2c_read(i2c_addr, addr, sizeof(addr), data, sizeof(data)) == 0) {
        *value = (data[3] << 24) + (data[2] << 16) + (data[1] << 8) + data[0];
        CTRL_DBG("read: %02x 0x%04x -> 0x%08x\n", i2c_addr, regaddr, *value);
    } else {
        CAM_ERR("Failed i2c read %02x 0x%04x\n", i2c_addr, regaddr);
        return -1;
    }
    return 0;
}

/* 16b aligned 2 bytes value register read */
int mhb_camera_i2c_read_reg2_16(uint16_t i2c_addr, uint16_t regaddr,
                                 uint16_t *value)
{
    uint8_t addr[2];
    uint8_t data[2];

    addr[0] = (regaddr >> 8) & 0xFF;
    addr[1] = regaddr & 0xFF;

    memset(data, 0, sizeof(data));

    if (mhb_camera_i2c_read_rs(i2c_addr, addr, sizeof(addr), data, sizeof(data)) == 0) {
        *value = (data[0] << 8) + data[1];
        CTRL_DBG("read: %02x 0x%04x -> 0x%04x\n", i2c_addr, regaddr, *value);
    } else {
        CAM_ERR("Failed i2c read %02x 0x%04x\n", i2c_addr, regaddr);
        return -1;
    }

    return 0;
}

/* 16b aligned 4 bytes value register read */
int mhb_camera_i2c_read_reg4_16(uint16_t i2c_addr, uint16_t regaddr,
                                 uint32_t *value)
{
    uint8_t addr[2];
    uint8_t data[4];

    addr[0] = (regaddr >> 8) & 0xFF;
    addr[1] = regaddr & 0xFF;

    memset(data, 0, sizeof(data));

    if (mhb_camera_i2c_read_rs(i2c_addr, addr, sizeof(addr), data, sizeof(data)) == 0) {
        *value = (data[0] << 8) + data[1] + (data[2] << 24) + (data[3] << 16);
        CTRL_DBG("read: %02x 0x%04x -> 0x%08x\n", i2c_addr, regaddr, *value);
    } else {
        CAM_ERR("Failed i2c read %02x 0x%04x\n", i2c_addr, regaddr);
        return -1;
    }

    return 0;
}

/* 1 bytes value register write */
int mhb_camera_i2c_write_reg1(uint16_t i2c_addr, uint16_t regaddr, uint8_t data)
{
    uint8_t addr[3];

    CTRL_DBG("write 0x%02x to %02x addr 0x%04x\n", data, i2c_addr, regaddr);
    addr[0] = regaddr & 0xFF;
    addr[1] = (regaddr >> 8) & 0xFF;
    addr[2] = data & 0xFF;

    int ret = mhb_camera_i2c_write(i2c_addr, addr, sizeof(addr));
    if (ret != 0) {
        CAM_ERR("Failed i2c write 0x%02x to %02x  addr 0x%04x err %d\n",
                data, i2c_addr, regaddr, ret);
    }

    return ret;
}

/* 2 bytes value register write */
int mhb_camera_i2c_write_reg2(uint16_t i2c_addr, uint16_t regaddr, uint16_t data)
{
    uint8_t addr[4];

    CTRL_DBG("write 0x%04x to %02x addr 0x%04x\n", data, i2c_addr, regaddr);
    addr[0] = regaddr & 0xFF;
    addr[1] = (regaddr >> 8) & 0xFF;
    addr[2] = data & 0xFF;
    addr[3] = (data >> 8) & 0xFF;

    int ret = mhb_camera_i2c_write(i2c_addr, addr, sizeof(addr));
    if (ret != 0) {
        CAM_ERR("Failed i2c write 0x%04x to %02x  addr 0x%04x err %d\n",
                data, i2c_addr, regaddr, ret);
    }

    return ret;
}

/* 4 bytes value register write */
int mhb_camera_i2c_write_reg4(uint16_t i2c_addr, uint16_t regaddr, uint32_t data)
{
    uint8_t addr[6];

    CTRL_DBG("write 0x%08x to %02x addr 0x%04x\n", data, i2c_addr, regaddr);
    addr[0] = regaddr & 0xFF;
    addr[1] = (regaddr >> 8) & 0xFF;
    addr[2] = data & 0xFF;
    addr[3] = (data >> 8) & 0xFF;
    addr[4] = (data >> 16) & 0xFF;
    addr[5] = (data >> 24) & 0xFF;

    int ret = mhb_camera_i2c_write(i2c_addr, addr, sizeof(addr));
    if (ret != 0) {
        CAM_ERR("Failed i2c write 0x%08x to %02x  addr 0x%04x err %d\n",
                data, i2c_addr, regaddr, ret);
    }

    return ret;
}

/* 16b aligned 2 bytes value register write */
int mhb_camera_i2c_write_reg2_16(uint16_t i2c_addr, uint16_t regaddr, uint16_t data)
{
    uint8_t addr[4];

    CTRL_DBG("write 0x%04x to %02x addr 0x%04x\n", data, i2c_addr, regaddr);
    addr[0] = (regaddr >> 8) & 0xFF;
    addr[1] = regaddr & 0xFF;
    addr[2] = (data >> 8) & 0xFF;
    addr[3] = data & 0xFF;

    int ret = mhb_camera_i2c_write(i2c_addr, addr, sizeof(addr));
    if (ret != 0) {
        CAM_ERR("Failed i2c write 0x%04x to %02x  addr 0x%04x err %d\n",
                data, i2c_addr, regaddr, ret);
    }

    return ret;
}

/* 16b aligned 4 bytes value register write */
int mhb_camera_i2c_write_reg4_16(uint16_t i2c_addr, uint16_t regaddr, uint32_t data)
{
    uint8_t addr[6];

    CTRL_DBG("write 0x%08x to %02x addr 0x%04x\n", data, i2c_addr, regaddr);
    addr[0] = (regaddr >> 8) & 0xFF;
    addr[1] = regaddr & 0xFF;
    addr[2] = (data >> 8) & 0xFF;
    addr[3] = data & 0xFF;
    addr[4] = (data >> 24) & 0xFF;
    addr[5] = (data >> 16) & 0xFF;

    int ret = mhb_camera_i2c_write(i2c_addr, addr, sizeof(addr));
    if (ret != 0) {
        CAM_ERR("Failed i2c write 0x%08x to %02x  addr 0x%04x err %d\n",
                data, i2c_addr, regaddr, ret);
    }

    return ret;
}

/* MHB Ops */

static int _mhb_camera_wait_for_response(pthread_cond_t *cond,
                                         uint16_t wait_event, char *str)
{
    int result;
    struct timespec expires;

    if (clock_gettime(CLOCK_REALTIME, &expires)) {
        return -EBADF;
    }

    uint64_t new_ns = timespec_to_nsec(&expires);
    new_ns += MHB_CDSI_OP_TIMEOUT_NS;
    nsec_to_timespec(new_ns, &expires);

    pthread_mutex_lock(&s_mhb_camera.mutex);
    s_mhb_camera.mhb_wait_event = wait_event;
    result = pthread_cond_timedwait(cond, &s_mhb_camera.mutex, &expires);
    if (!result)
        result = s_mhb_camera.mhb_wait_event;
    s_mhb_camera.mhb_wait_event = MHB_CAM_WAIT_EV_NONE;
    pthread_mutex_unlock(&s_mhb_camera.mutex);

    clock_gettime(CLOCK_REALTIME, &expires);
    new_ns = timespec_to_nsec(&expires) - (new_ns - MHB_CDSI_OP_TIMEOUT_NS);
    CAM_ERR("%s: Time spent on %s %dms Result %4x\n", result?"ERROR":"INFO",
            str, (uint16_t)(new_ns/NSEC_PER_MSEC), result);

    return result;
}

static int _mhb_camera_slave_status_callback(struct device *dev, uint32_t slave_status)
{
    s_mhb_camera.apbe_state = slave_status;

    pthread_mutex_lock(&s_mhb_camera.mutex);
    CAM_DBG("slave status %d wait_event=0x%04x\n",
            slave_status, s_mhb_camera.mhb_wait_event);

    if (s_mhb_camera.mhb_wait_event == (slave_status|MHB_CAM_PWRCTL_WAIT_MASK)) {
         s_mhb_camera.mhb_wait_event = MHB_CAM_WAIT_EV_NONE;
         pthread_cond_signal(&s_mhb_camera.slave_cond);
    }
    pthread_mutex_unlock(&s_mhb_camera.mutex);
    return 0;
}

static int _mhb_camera_set_apbe_state(int state)
{
    int ret;

    ret = device_slave_pwrctrl_send_slave_state(s_mhb_camera.slave_pwr_ctrl, state);
    if(ret) {
        CAM_ERR("ERROR: Failed to set slave state %d\n", ret);
        return ret;
    }

    return ret;
}

static int _mhb_csi_camera_config_req(uint8_t *cfg, size_t cfg_size)
{
    struct mhb_hdr hdr;
    int result = -1;

    memset(&hdr, 0, sizeof(hdr));
    hdr.addr = MHB_ADDR_CDSI1;
    hdr.type = MHB_TYPE_CDSI_CONFIG_REQ;

    result = device_mhb_send(s_mhb_camera.mhb_device, &hdr, (uint8_t *)cfg, cfg_size, 0);
    if (result) {
        CAM_ERR("ERROR: failed to send mhb config req. result %d\n", result);
        return result;
    }

    return result;
}

static int _mhb_csi_camera_unconfig_req(void)
{
    struct mhb_hdr hdr;
    int result = 0;

    memset(&hdr, 0, sizeof(hdr));
    hdr.addr = MHB_ADDR_CDSI1;
    hdr.type = MHB_TYPE_CDSI_UNCONFIG_REQ;

    result = device_mhb_send(s_mhb_camera.mhb_device, &hdr, NULL, 0, 0);
    if (result) {
        CAM_ERR("ERROR: failed to send mhb unconfig req. result %d\n", result);
        return result;
    }

    return result;
}


static int _mhb_csi_camera_control_req(uint8_t command)
{
    struct mhb_hdr hdr;
    struct mhb_cdsi_control_req req;
    int result = 0;

    memset(&hdr, 0, sizeof(hdr));
    hdr.addr = MHB_ADDR_CDSI1;
    hdr.type = MHB_TYPE_CDSI_CONTROL_REQ;

    req.command = command;

    result = device_mhb_send(s_mhb_camera.mhb_device, &hdr, (uint8_t *)&req, sizeof(req), 0);
    if (result) {
        CAM_ERR("ERROR: failed to send mhb ctrl cmd %d result %d\n", command, result);
        return result;
    }

    return result;
}

static int _mhb_csi_camera_handle_msg(struct device *dev,
    struct mhb_hdr *hdr, uint8_t *payload, size_t payload_length)
{
    if (hdr->addr == MHB_ADDR_CDSI1) {
        switch (hdr->type) {
        case MHB_TYPE_CDSI_WRITE_CMDS_RSP:
        case MHB_TYPE_CDSI_READ_CMDS_RSP:
        case MHB_TYPE_CDSI_STATUS_RSP:
            if (hdr->result)
                CAM_ERR("ERROR:failed cmd: addr=%d type=%d result=%d\n",
                        hdr->addr, hdr->type, hdr->result);
            break;
        case MHB_TYPE_CDSI_UNCONFIG_RSP:
        case MHB_TYPE_CDSI_CONFIG_RSP:
        case MHB_TYPE_CDSI_CONTROL_RSP:
            pthread_mutex_lock(&s_mhb_camera.mutex);
            if (s_mhb_camera.mhb_wait_event == (hdr->type|MHB_CAM_CDSI_WAIT_MASK)) {
                if (hdr->result == MHB_RESULT_SUCCESS) {
                    s_mhb_camera.mhb_wait_event = MHB_CAM_WAIT_EV_NONE;
                }
                pthread_cond_signal(&s_mhb_camera.cdsi_cond);
            }
            pthread_mutex_unlock(&s_mhb_camera.mutex);
            break;
        default:
            CAM_ERR("unexpected rsp: addr=%d type=%d result=%d wait=0x%04x\n",
                    hdr->addr, hdr->type,
                    hdr->result, s_mhb_camera.mhb_wait_event);
        }
    }

    return 0;
}

mhb_camera_sm_event_t mhb_camera_power_on(void)
{
    CAM_DBG("\n");

    if(s_mhb_camera.apbe_state != MHB_PM_STATUS_PEER_CONNECTED) {
        if(_mhb_camera_set_apbe_state(SLAVE_STATE_ENABLED)) {
            CAM_ERR("Failed to turn ON APBE\n");
            return MHB_CAMERA_EV_FAIL;
        }
    }

    if(s_mhb_camera.apbe_state != MHB_PM_STATUS_PEER_CONNECTED &&
       s_mhb_camera.apbe_state != MHB_PM_STATUS_PEER_ON) {
        if (_mhb_camera_wait_for_response(&s_mhb_camera.slave_cond,
                MHB_CAM_PWRCTL_WAIT_MASK|MHB_PM_STATUS_PEER_ON, "PEER ON")) {
            CAM_ERR("Failed waiting for PEER_ON\n");
            return MHB_CAMERA_EV_FAIL;
        }
    }

    if (_mhb_camera_soc_enable()) {
        CAM_ERR("Failed to turn on Camera SOC\n");
        return MHB_CAMERA_EV_FAIL;
    }

    if(s_mhb_camera.apbe_state != MHB_PM_STATUS_PEER_CONNECTED) {
        if (_mhb_camera_wait_for_response(&s_mhb_camera.slave_cond,
                MHB_CAM_PWRCTL_WAIT_MASK|MHB_PM_STATUS_PEER_CONNECTED,
                "PEER CONNECTED")) {
            CAM_ERR("Failed waiting for PEER_CONNECTED\n");
            return MHB_CAMERA_EV_FAIL;
        }
    }

    s_mhb_camera.soc_enabled = 1;
    return MHB_CAMERA_EV_POWERED_ON;
}

mhb_camera_sm_event_t mhb_camera_power_off(void)
{
    CAM_DBG("\n");

    if(_mhb_camera_set_apbe_state(SLAVE_STATE_DISABLED))
        CAM_ERR("Failed to turn OFF APBE\n");
    s_mhb_camera.apbe_state = MHB_PM_STATUS_PEER_DISCONNECTED;

    _mhb_camera_soc_disable();
    s_mhb_camera.soc_enabled = 0;

    return MHB_CAMERA_EV_NONE;
}

mhb_camera_sm_event_t mhb_camera_stream_on(void)
{
    const struct camera_ext_format_db *db = camera_ext_get_format_db();
    const struct camera_ext_format_user_config *cfg = camera_ext_get_user_config();
    const struct camera_ext_format_node *fmt;
    const struct camera_ext_frmsize_node *frmsize;
    const struct camera_ext_frmival_node *ival;

    CAM_DBG("\n");

    fmt = get_current_format_node(db, cfg);
    if (fmt == NULL) {
        CAM_ERR("Failed to get current format\n");
        return MHB_CAMERA_EV_FAIL;
    }

    frmsize = get_current_frmsize_node(db, cfg);
    if (frmsize == NULL) {
        CAM_ERR("Failed to get current frame size\n");
        return MHB_CAMERA_EV_FAIL;
    }

    ival = get_current_frmival_node(db, cfg);
    if (ival == NULL) {
        CAM_ERR("Failed to get current frame interval\n");
        return MHB_CAMERA_EV_FAIL;
    }

    if (ival->user_data == NULL) {
        CAM_ERR("Failed to get user data\n");
        return MHB_CAMERA_EV_FAIL;
    }

    switch(fmt->fourcc) {
        case V4L2_PIX_FMT_RGB24:
            mhb_camera_csi_config.bpp = 24;
            break;
        case V4L2_PIX_FMT_UYVY:
            mhb_camera_csi_config.bpp = 16;
            break;
        default:
            CAM_ERR("Unsupported format 0x%x\n", fmt->fourcc);
            return MHB_CAMERA_EV_FAIL;
    }

    mhb_camera_csi_config.width = frmsize->width;
    mhb_camera_csi_config.height = frmsize->height;
    mhb_camera_csi_config.rx_num_lanes = _mhb_camera_get_csi_rx_lanes(ival->user_data);
    mhb_camera_csi_config.framerate = roundf((float)(ival->denominator) /
                                             (float)(ival->numerator));
    mhb_camera_csi_config.tx_bits_per_lane = 500000000;
    mhb_camera_csi_config.rx_bits_per_lane = 500000000;

    /* Send the configuration to the APBE. */
    // TODO: ERROR HANDLING

    _mhb_camera_stream_configure();

    if (_mhb_csi_camera_config_req((uint8_t*)&mhb_camera_csi_config,
                                    sizeof(mhb_camera_csi_config))) {
        CAM_ERR("ERROR: send config failed\n");
    }
    _mhb_camera_wait_for_response(&s_mhb_camera.cdsi_cond,
                                  MHB_CAM_CDSI_WAIT_MASK|MHB_TYPE_CDSI_CONFIG_RSP,
                                  "CDSI CONFIG");

    _mhb_camera_stream_enable();

    if (_mhb_csi_camera_control_req(MHB_CDSI_COMMAND_START)) {
        CAM_ERR("ERROR: start failed\n");
    }
    _mhb_camera_wait_for_response(&s_mhb_camera.cdsi_cond,
                                  MHB_CAM_CDSI_WAIT_MASK|MHB_TYPE_CDSI_CONTROL_RSP,
                                  "CDSI START");


    return MHB_CAMERA_EV_CONFIGURED;
}

mhb_camera_sm_event_t mhb_camera_stream_off(void)
{
    CAM_DBG("\n");
    // TODO: ERROR HANDLING
    _mhb_csi_camera_control_req(MHB_CDSI_COMMAND_STOP);
    _mhb_csi_camera_unconfig_req();
    _mhb_camera_wait_for_response(&s_mhb_camera.cdsi_cond,
                                  MHB_CAM_CDSI_WAIT_MASK|MHB_TYPE_CDSI_CONTROL_RSP,
                                  "CDSI STOP");

    _mhb_camera_wait_for_response(&s_mhb_camera.cdsi_cond,
                                  MHB_CAM_CDSI_WAIT_MASK|MHB_TYPE_CDSI_UNCONFIG_RSP,
                                  "CDSI UNCONFIG");

    _mhb_camera_stream_disable();

    return MHB_CAMERA_EV_DECONFIGURED;
}

/* CAMERA_EXT devops */
static int _power_on(struct device *dev)
{
    CAM_DBG("mhb_camera_csi\n");
    return mhb_camera_sm_execute(MHB_CAMERA_EV_POWER_ON_REQ);
}

static int _power_off(struct device *dev)
{
    CAM_DBG("mhb_camera_csi\n");
    return mhb_camera_sm_execute(MHB_CAMERA_EV_POWER_OFF_REQ);
}

static int _stream_on(struct device *dev)
{
    CAM_DBG("mhb_camera_csi\n");
    return mhb_camera_sm_execute(MHB_CAMERA_EV_STREAM_ON_REQ);
}

static int _stream_off(struct device *dev)
{
    CAM_DBG("mhb_camera_csi\n");
    return mhb_camera_sm_execute(MHB_CAMERA_EV_STREAM_OFF_REQ);
}

static int _dev_probe(struct device *dev)
{
    CAM_DBG("mhb_camera_csi\n");

    struct mhb_camera_s* mhb_camera = &s_mhb_camera;
    device_set_private(dev, (void*)mhb_camera);
    /* Only initialize I2C once in life */
    if (mhb_camera->cam_i2c == NULL) {
        /* initialize once */
        mhb_camera->cam_i2c = up_i2cinitialize(CONFIG_CAMERA_I2C_BUS);

        if (mhb_camera->cam_i2c == NULL) {
            CAM_ERR("Failed to initialize I2C\n");
            return -1;
        }
    }

    pthread_mutex_init(&mhb_camera->mutex, NULL);
    pthread_mutex_init(&i2c_mutex, NULL);

    pthread_cond_init(&mhb_camera->slave_cond, NULL);
    pthread_cond_init(&mhb_camera->cdsi_cond, NULL);

    mhb_camera->soc_enabled = 0;
    mhb_camera->apbe_state = MHB_PM_STATUS_PEER_DISCONNECTED;
    mhb_camera->mhb_wait_event = MHB_CAM_WAIT_EV_NONE;
    _mhb_camera_init(dev);
    camera_ext_register_format_db(&mhb_camera_format_db);
    camera_ext_register_control_db(&mhb_camera_ctrl_db);

    return 0;
}

static int _dev_open(struct device *dev)
{
    struct mhb_camera_s* mhb_camera = (struct mhb_camera_s*)device_get_private(dev);
    int ret;
    CAM_DBG("mhb_camera_csi\n");

    if (mhb_camera->mhb_device) {
        CAM_ERR("ERROR: already opened.\n");
        return -EBUSY;
    }

    mhb_camera->slave_pwr_ctrl = device_open(DEVICE_TYPE_SLAVE_PWRCTRL_HW, MHB_ADDR_CDSI1);
    if (!mhb_camera->slave_pwr_ctrl) {
       CAM_ERR("ERROR: Failed to open SLAVE Power Control\n");
       return -ENODEV;
    }

    mhb_camera->mhb_device = device_open(DEVICE_TYPE_MHB, MHB_ADDR_CDSI1);
    if (!mhb_camera->mhb_device) {
        CAM_ERR("ERROR: failed to open MHB device.\n");
        device_close(mhb_camera->slave_pwr_ctrl);
        mhb_camera->slave_pwr_ctrl = NULL;
        return -ENODEV;
    }
    mhb_camera_sm_init();

    ret = device_slave_pwrctrl_register_status_callback(mhb_camera->slave_pwr_ctrl,
                                                        _mhb_camera_slave_status_callback);
    if(ret) {
        CAM_ERR("ERROR: Failed to register callback %d\n", ret);
        return ret;
    }

    ret = device_mhb_register_receiver(mhb_camera->mhb_device, MHB_ADDR_CDSI1,
                                 _mhb_csi_camera_handle_msg);

    if(ret) {
        CAM_ERR("ERROR: Failed to register device_mhb_register_receiver %d\n", ret);
        return ret;
    }
    return 0;
}

static void _dev_close(struct device *dev)
{
    CAM_DBG("mhb_camera_csi\n");
    struct mhb_camera_s* mhb_camera = (struct mhb_camera_s*)device_get_private(dev);

    if (mhb_camera->mhb_device) {
        device_mhb_unregister_receiver(mhb_camera->mhb_device, MHB_ADDR_CDSI1,
                                       _mhb_csi_camera_handle_msg);
    }

    if (mhb_camera->slave_pwr_ctrl) {
        device_slave_pwrctrl_unregister_status_callback(mhb_camera->slave_pwr_ctrl,
                                                        _mhb_camera_slave_status_callback);
    }

    device_close(mhb_camera->mhb_device);
    device_close(mhb_camera->slave_pwr_ctrl);
    mhb_camera->mhb_device = NULL;
    mhb_camera->slave_pwr_ctrl = NULL;
}

static int _mhb_camera_ext_ready_ctrl_set(struct device *dev,
    uint32_t idx, uint8_t *ctrl_val, uint32_t ctrl_val_size)
{
    uint8_t state = mhb_camera_sm_get_state();
    if (state == MHB_CAMERA_STATE_OFF ||
        state == MHB_CAMERA_STATE_WAIT_OFF) {
        CAM_ERR("ERROR: Camera Off : State %d\n", state);
        return -ENODEV;
    }
    if (!s_mhb_camera.soc_enabled) {
        CAM_DBG("Cam not ready, try again. CtrlID 0x%08x State %s\n",
                idx, mhb_camera_sm_state_str(state));

        usleep(CTRL_SET_RETRY_DELAY_US);
        return -EAGAIN;
    }
    return camera_ext_ctrl_set(dev, idx, ctrl_val, ctrl_val_size);
}

static struct device_camera_ext_dev_type_ops camera_ext_type_ops = {
    .register_event_cb = camera_ext_register_event_cb,
    .power_on          = _power_on,
    .power_off         = _power_off,
    .stream_on         = _stream_on,
    .stream_off        = _stream_off,
    .input_enum        = camera_ext_input_enum,
    .input_get         = camera_ext_input_get,
    .input_set         = camera_ext_input_set,
    .format_enum       = camera_ext_format_enum,
    .format_get        = camera_ext_format_get,
    .format_set        = camera_ext_format_set,
    .frmsize_enum      = camera_ext_frmsize_enum,
    .frmival_enum      = camera_ext_frmival_enum,
    .stream_set_parm   = camera_ext_stream_set_parm,
    .stream_get_parm   = camera_ext_stream_get_parm,
    .ctrl_get_cfg      = camera_ext_ctrl_get_cfg,
    .ctrl_get          = camera_ext_ctrl_get,
    .ctrl_set          = _mhb_camera_ext_ready_ctrl_set,
    .ctrl_try          = camera_ext_ctrl_try,
};

static struct device_driver_ops camera_ext_driver_ops = {
    .probe    = _dev_probe,
    .open     = _dev_open,
    .close    = _dev_close,
    .type_ops = &camera_ext_type_ops,
};

struct device_driver cam_ext_mhb_driver = {
    .type = DEVICE_TYPE_CAMERA_EXT_HW,
    .name = "Motorola",
    .desc = "Motorola MHB Camera",
    .ops  = &camera_ext_driver_ops,
};