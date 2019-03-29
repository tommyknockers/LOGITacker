#include <string.h> // for memcpy
#include "nrf.h"
#include "flash_device_info.h"
#include "fds.h"
#include "unifying.h"
#include "nrf_esb_illegalmod.h"
#include "nrf_log.h"
#include "nrf_delay.h"
#include "radio.h"
#include "app_timer.h"
#include "timestamp.h"
#include "app_scheduler.h"


APP_TIMER_DEF(m_timer_tx_record);
static unifying_rf_record_set_t m_record_sets[NRF_ESB_PIPE_COUNT];
static unifying_rf_record_t m_records_from_sets[NRF_ESB_PIPE_COUNT][UNIFYING_MAX_STORED_REPORTS_PER_PIPE];

uint32_t restoreDeviceInfoFromFlash(uint16_t deviceRecordIndex, device_info_t *deviceInfo) {
    ret_code_t ret;
    uint16_t recordIdx = FLASH_RECORD_PREFIX_DEVICES_DEVICE_INFO | deviceRecordIndex;

    //Try to load first state record from flash, create if not existing
    fds_record_desc_t desc = {0}; //used as ref to current record
    fds_find_token_t  tok  = {0}; //used for find functions if records with same ID exist and we want to find next one

    ret = fds_record_find(FLASH_FILE_DEVICES, recordIdx, &desc, &tok);
    if (ret != FDS_SUCCESS) { return ret; }

    /* A config file is in flash. Let's update it. */
    fds_flash_record_t record_state = {0};

    // Open the record and read its contents. */
    ret = fds_record_open(&desc, &record_state);
    if (ret != FDS_SUCCESS) { return ret; }

    // restore state from flash into state (copy)
    memcpy(deviceInfo, record_state.p_data, sizeof(device_info_t));

    return fds_record_close(&desc);
}

uint32_t restoreDeviceWhitenedReportsFromFlash(uint16_t deviceRecordIndex, whitened_replay_frames_t *reports) {
    ret_code_t ret;
    uint16_t recordIdx = FLASH_RECORD_PREFIX_DEVICES_DEVICE_WHITENED_KEY_REPORTS | deviceRecordIndex;

    //Try to load first state record from flash, create if not existing
    fds_record_desc_t desc = {0}; //used as ref to current record
    fds_find_token_t  tok  = {0}; //used for find functions if records with same ID exist and we want to find next one

    ret = fds_record_find(FLASH_FILE_DEVICES, recordIdx, &desc, &tok);
    if (ret != FDS_SUCCESS) { return ret; }

    /* A config file is in flash. Let's update it. */
    fds_flash_record_t record_state = {0};

    // Open the record and read its contents. */
    ret = fds_record_open(&desc, &record_state);
    if (ret != FDS_SUCCESS) { return ret; }

    // restore state from flash into state (copy)
    memcpy(reports, record_state.p_data, sizeof(whitened_replay_frames_t));

    return fds_record_close(&desc);
}

uint32_t updateDeviceInfoOnFlash(uint16_t deviceRecordIndex, device_info_t *deviceInfo) {
    ret_code_t ret;
    uint16_t recordIdx = FLASH_RECORD_PREFIX_DEVICES_DEVICE_INFO | deviceRecordIndex;
    
    // flash record for device info
    fds_record_t flash_record = {
        .file_id           = FLASH_FILE_DEVICES,
        .key               = recordIdx,
        .data.p_data       = deviceInfo,
        // The length of a record is always expressed in 4-byte units (words).
        .data.length_words = (sizeof(*deviceInfo) + 3) / sizeof(uint32_t),
    };
    

    //Try to load first state record from flash, create if not existing
    fds_record_desc_t desc = {0}; //used as ref to current record
    fds_find_token_t  tok  = {0}; //used for find functions if records with same ID exist and we want to find next one

    ret = fds_record_find(FLASH_FILE_DEVICES, recordIdx, &desc, &tok);
    if (ret == FDS_SUCCESS) { 
        // Write the updated record to flash.
        return fds_record_update(&desc, &flash_record);
    } else {
        // no found, create new flash record
        return fds_record_write(&desc, &flash_record);
    }
}

uint32_t updateDeviceWhitenedReportsOnFlash(uint16_t deviceRecordIndex, whitened_replay_frames_t *reports) {
    ret_code_t ret;
    uint16_t recordIdx = FLASH_RECORD_PREFIX_DEVICES_DEVICE_WHITENED_KEY_REPORTS | deviceRecordIndex;

    // flash record for whitened reports
    fds_record_t flash_record = {
        .file_id           = FLASH_FILE_DEVICES,
        .key               = recordIdx,
        .data.p_data       = reports,
        // The length of a record is always expressed in 4-byte units (words).
        .data.length_words = (sizeof(*reports) + 3) / sizeof(uint32_t),
    };
    
    //Try to load first state record from flash, create if not existing
    fds_record_desc_t desc = {0}; //used as ref to current record
    fds_find_token_t  tok  = {0}; //used for find functions if records with same ID exist and we want to find next one

    ret = fds_record_find(FLASH_FILE_DEVICES, recordIdx, &desc, &tok);
    if (ret == FDS_SUCCESS) { 
        // Write the updated record to flash.
        return fds_record_update(&desc, &flash_record);
    } else {
        // no found, create new flash record
        return fds_record_write(&desc, &flash_record);
    }
}

uint8_t unifying_calculate_checksum(uint8_t * p_array, uint8_t paylen) {
/*
	chksum := byte(0xff)
	for i := 0; i < len(payload)-1; i++ {
		chksum = (chksum - payload[i]) & 0xff
	}
	chksum = (chksum + 1) & 0xff

	payload[len(payload)-1] = chksum

*/

    uint8_t checksum = 0x00;
    for (int i = 0; i < paylen; i++) {
        checksum -= p_array[i];
    }
    //checksum++;

    return checksum;
}

bool unifying_validate_payload(uint8_t * p_array, uint8_t paylen) {
    if (paylen < 1) return false;
    uint8_t chksum = unifying_calculate_checksum(p_array, paylen);
    p_array[0] = chksum;

    return true;
}

bool unifying_payload_update_checksum(uint8_t * p_array, uint8_t paylen) {
    if (paylen < 1) return false;
    uint8_t chksum = unifying_calculate_checksum(p_array, paylen-1);
    p_array[paylen-1] = chksum;

    return true;
}

uint32_t unifyingExtractCounterFromEncKbdFrame(nrf_esb_payload_t frame, uint32_t *p_counter) {
    // assure frame is encrypted keyboard
    if (frame.length != 22) return NRF_ERROR_INVALID_LENGTH;
    if ((frame.data[1] & 0x1f) != UNIFYING_RF_REPORT_ENCRYPTED_KEYBOARD) return NRF_ERROR_INVALID_DATA;
    *p_counter = frame.data[10] << 24 | frame.data[11] << 16 | frame.data[12] << 8 | frame.data[13];
    return NRF_SUCCESS;
}

#define UNIFYING_MIN_STORED_REPORTS_VALID_PER_PIPE 26

bool validate_record_buf_successive_keydown_keyrelease(uint8_t pipe) {
    unifying_rf_record_set_t *p_rs = &m_record_sets[pipe];
    // walk buffer backwards starting at last recorded frame
    int end_pos = (int) p_rs->write_pos - 1;
    if (end_pos < 0) end_pos += UNIFYING_MAX_STORED_REPORTS_PER_PIPE;
    NRF_LOG_INFO("end_pos %d", end_pos);
    int check_pos = end_pos;
    bool wantKeyRelease = true;
    uint32_t wantedCounter = p_rs->records[check_pos].counter; 
    unifying_rf_record_t* p_current_record;
    int valid_count = 0;
    for (int i=0; i<UNIFYING_MAX_STORED_REPORTS_PER_PIPE; i++) {
        check_pos = end_pos - i;
        if (check_pos < 0) check_pos += UNIFYING_MAX_STORED_REPORTS_PER_PIPE;
        
        p_current_record = &p_rs->records[check_pos];

        //check if encrypted keyboard frame, skip otherwise
        if (p_current_record->reportType != UNIFYING_RF_REPORT_ENCRYPTED_KEYBOARD) continue;
        NRF_LOG_DEBUG("frame at pos %d has valid report type", check_pos);

        //check if key up/release in alternating order
        if (p_current_record->isEncrytedKeyRelease != wantKeyRelease) break;
        // toggle key release check for next iteration
        wantKeyRelease = !wantKeyRelease;
        NRF_LOG_DEBUG("frame at pos %d has valid key down/release type (key release %d)", check_pos, wantKeyRelease);

        //check counter
        if (p_current_record->counter != wantedCounter) break;
        NRF_LOG_DEBUG("frame at pos %d has valid counter %08x", check_pos, wantedCounter);
        // decrement counter for next iteration
        if (wantedCounter == 0) return false; //<--- should barely happen
        wantedCounter--;

        //if here, the recorded frame is valid
        valid_count = i;

        //success condition
        if (valid_count >= UNIFYING_MIN_STORED_REPORTS_VALID_PER_PIPE) {
            int start_pos = end_pos - valid_count;
            if (start_pos < 0) start_pos += UNIFYING_MAX_STORED_REPORTS_PER_PIPE; //account for ring buffer style of record buffer

            // update record set
            p_rs->first_pos = (uint8_t) start_pos;
            p_rs->last_pos = (uint8_t) end_pos;
            p_rs->read_pos = p_rs->first_pos;
            NRF_LOG_INFO("found enough (%d) valid frames, start %d end %d", valid_count, start_pos, end_pos);
            return true;
        }
    }

    NRF_LOG_INFO("found %d valid key down/up frames, starting from buffer end at %d", valid_count, end_pos);

    return false;
}


uint32_t timestamp_last = 0;
uint32_t timestamp = 0;
uint32_t timestamp_delta = 0;
// Purpose of the method is to store captured RF frames and return true, once enough frames for
// replay are recorded
// The method needs to keep track of frame sequences, to different distinguish record types
// (f.e. a encrypted key-release frame is succeeded by set-keep-alive frames, while a key down frame
// isn't)

// ToDo: fix .. timestamp_delta doesn't account for changing pipes between successive method calls
bool unifying_record_rf_frame(nrf_esb_payload_t frame) {
    bool result = false;
    if (frame.length < 5) return false; //ToDo: with error
    
    unifying_rf_record_set_t *p_rs = &m_record_sets[frame.pipe];

    if (p_rs->disallowWrite) {
        return false;
    }



    uint8_t frameType;
    bool isKeepAlive;
    unifying_frame_classify(frame, &frameType, &isKeepAlive);
    
    // ignore successive SetKeepAlive frames
    if (frameType == UNIFYING_RF_REPORT_SET_KEEP_ALIVE && m_record_sets[p_rs->pipe_num].lastRecordedReportType == UNIFYING_RF_REPORT_SET_KEEP_ALIVE) return false;
    
    bool storeFrame = false;
    // in case the RF frame is of set-keep-alive type, we don't store the report, but mark a possible successive
    // encrypted keyboard report as "key release" frame
    switch (frameType) {
        case UNIFYING_RF_REPORT_SET_KEEP_ALIVE:
        {
            // we have a set keep alive frame, if previous RF frame was an encrypted keyboard report, mark as key release
            uint8_t previous_pos = p_rs->write_pos == 0 ? UNIFYING_MAX_STORED_REPORTS_PER_PIPE-1 : p_rs->write_pos - 1;
            unifying_rf_record_t* p_previous_record = &p_rs->records[previous_pos];
            if (!p_previous_record->isEncrytedKeyRelease && p_previous_record->reportType == UNIFYING_RF_REPORT_ENCRYPTED_KEYBOARD) {
                p_previous_record->isEncrytedKeyRelease = true;
                NRF_LOG_INFO("last recorded encrypted keyboard frame marked as key release frame")
            } 
            
            break;
        }
        
        case UNIFYING_RF_REPORT_ENCRYPTED_KEYBOARD:
        {
            storeFrame = true;
            break;
        }
        default:
            return false; //ignore frame
    }

    m_record_sets[p_rs->pipe_num].lastRecordedReportType = frame.data[1] & UNIFYING_RF_REPORT_TYPE_MSK;

    if (storeFrame) {
        uint8_t pos = p_rs->write_pos;
        unifying_rf_record_t* p_record = &p_rs->records[pos];
        p_record->length = frame.length;
        p_record->reportType = frame.data[1] & UNIFYING_RF_REPORT_TYPE_MSK;
        memcpy(p_record->data, frame.data, frame.length);
        if (p_record->reportType == UNIFYING_RF_REPORT_ENCRYPTED_KEYBOARD) {
            unifyingExtractCounterFromEncKbdFrame(frame, &p_record->counter);
        }

        timestamp_last = timestamp;
        timestamp = timestamp_get();
        timestamp_delta = timestamp - timestamp_last;
        p_record->pre_delay_ms = timestamp_delta;
        if (pos == 0) p_record->pre_delay_ms = 0; //no delay for first record

        NRF_LOG_INFO("Frame recorded stored at timer count %d, last %d, diff %d", timestamp, timestamp_last, timestamp_delta);

        pos++;
        if (pos >= UNIFYING_MAX_STORED_REPORTS_PER_PIPE) pos = 0;
        p_rs->write_pos = pos;
    }

    
    result = validate_record_buf_successive_keydown_keyrelease(frame.pipe);

    return result;
}

void unifying_frame_classify(nrf_esb_payload_t frame, uint8_t *p_outRFReportType, bool *p_outHasKeepAliveSet) { 
    //filter out frames < 5 byte length (likely ACKs)
    if (frame.length < 5) {
        p_outRFReportType = UNIFYING_RF_REPORT_INVALID;
        p_outHasKeepAliveSet = false;
        return;
    }

    *p_outRFReportType = frame.data[1]; // byte 1 is rf report type, byte 0 is device prefix
    *p_outHasKeepAliveSet = (frame.data[1] & 0x40) != 0;
    *p_outRFReportType &= UNIFYING_RF_REPORT_TYPE_MSK; //mask report type

    return;
}

void unifying_frame_classify_log(nrf_esb_payload_t frame) {     
    bool logKeepAliveEmpty = false;

    //filter out frames < 5 byte length (likely ACKs)
    if (frame.length < 5) {
        NRF_LOG_DEBUG("Invalid Unifying RF frame (wrong length or empty ack)");
        return;
    }

    uint8_t reportType = frame.data[1]; // byte 1 is rf report type, byte 0 is device prefix
    bool keepAliveSet = (reportType & 0x40) != 0;
    reportType &= UNIFYING_RF_REPORT_TYPE_MSK; //mask report type
    uint32_t counter;

    

    //filter out Unifying keep alive
    switch (reportType) {
        case UNIFYING_RF_REPORT_PLAIN_KEYBOARD:
            NRF_LOG_INFO("Unencrypted keyboard");
            return;
        case UNIFYING_RF_REPORT_PLAIN_MOUSE:
            NRF_LOG_INFO("Unencrypted mouse"); 
            return;
        case UNIFYING_RF_REPORT_PLAIN_MULTIMEDIA:
            NRF_LOG_INFO("Unencrypted multimedia key");
            return;
        case UNIFYING_RF_REPORT_PLAIN_SYSTEM_CTL:
            NRF_LOG_INFO("Unencrypted system control key");
            return;
        case UNIFYING_RF_REPORT_LED:
            NRF_LOG_INFO("LED (outbound)");
            return;
        case UNIFYING_RF_REPORT_SET_KEEP_ALIVE:
            NRF_LOG_INFO("Set keep-alive");
            return;
        case UNIFYING_RF_REPORT_HIDPP_SHORT:
            NRF_LOG_INFO("HID++ short");
            return;
        case UNIFYING_RF_REPORT_HIDPP_LONG:
            NRF_LOG_INFO("HID++ long");
            return;
        case UNIFYING_RF_REPORT_ENCRYPTED_KEYBOARD:
            //counter = frame.data[10] << 24 | frame.data[11] << 16 | frame.data[12] << 8 | frame.data[13];
            counter = 0;
            if (unifyingExtractCounterFromEncKbdFrame(frame, &counter) == NRF_SUCCESS) {
                NRF_LOG_INFO("Encrypted keyboard, counter %08x", counter);

            }

            return;
        case UNIFYING_RF_REPORT_PAIRING:
            NRF_LOG_INFO("Pairing");
            return;
        case 0x00:
            if (keepAliveSet && logKeepAliveEmpty) NRF_LOG_INFO("Empty keep alive");
            return;
        default:
            NRF_LOG_INFO("Unknown frame type %02x, keep alive %s", frame.data[1], keepAliveSet ? "set" : "not set");
            return;
    }
}

void timer_tx_record_from_scheduler(void *p_event_data, uint16_t event_size) {
    NRF_LOG_INFO("!!!!!!!!!!!!!!!REPLAY HANDLER")

    // process scheduled event for this handler in main loop during scheduler processing
    static nrf_esb_payload_t tx_payload; 
    unifying_rf_record_set_t* p_rs = (unifying_rf_record_set_t*) p_event_data;
    uint8_t current_record_pos = p_rs->read_pos;
    unifying_rf_record_t current_record = p_rs->records[current_record_pos];

    //transmit RF frame
    NRF_LOG_INFO("Replaying frame %d", current_record_pos);
    memcpy(tx_payload.data, current_record.data, current_record.length);
    tx_payload.length = current_record.length;
    tx_payload.pipe = p_rs->pipe_num;
    tx_payload.noack = false;

    if (radioTransmit(&tx_payload, true)) {
        //NRF_LOG_INFO("TX succeeded")
    } else {
        //NRF_LOG_INFO("TX failed")
    }
    
    
    //check if follow up record has to be scheduled
    if (current_record_pos != p_rs->last_pos) {
        // advance read_pos to next record
        p_rs->read_pos = current_record_pos+1; 
        if (p_rs->read_pos >= UNIFYING_MAX_STORED_REPORTS_PER_PIPE) p_rs->read_pos -= UNIFYING_MAX_STORED_REPORTS_PER_PIPE;

        uint32_t next_record_pre_delay_ms = p_rs->records[p_rs->read_pos].pre_delay_ms;
        if (next_record_pre_delay_ms == 0) next_record_pre_delay_ms++; //zero delay wouldn't triger timer at all

next_record_pre_delay_ms = 8;

        NRF_LOG_INFO("Next replay frame %d schedule for TX in %d ms", p_rs->read_pos, next_record_pre_delay_ms);
        app_timer_start(m_timer_tx_record, APP_TIMER_TICKS(next_record_pre_delay_ms), p_rs);
    } else {
        NRF_LOG_INFO("Replay finished");
        //p_rs->disallowWrite = false; //that's a copy
        m_record_sets[p_rs->pipe_num].disallowWrite = false;
        
    }
}

void timer_tx_record_to_scheduler(void* p_context) {
    //NRF_LOG_INFO("Forward replay timer event to scheduler");
    // schedule as event to main  (not isr) instead of executing directly
    app_sched_event_put(p_context, sizeof(unifying_rf_record_set_t), timer_tx_record_from_scheduler);
}

void unifying_transmit_records(uint8_t pipe_num) {
    unifying_rf_record_set_t* p_rs = &m_record_sets[pipe_num];
    uint8_t current_record = p_rs->first_pos;
    
    p_rs->disallowWrite = true;

    uint32_t delay_ms = p_rs->records[current_record].pre_delay_ms;
    if (delay_ms == 0) delay_ms++; //zero delay wouldn't triger timer at all
    app_timer_start(m_timer_tx_record, APP_TIMER_TICKS(delay_ms), p_rs);
    NRF_LOG_INFO("Replay timer started");
}

void unifying_init() {
    app_timer_create(&m_timer_tx_record, APP_TIMER_MODE_SINGLE_SHOT, timer_tx_record_to_scheduler);
    for (int i=0; i<NRF_ESB_PIPE_COUNT; i++) {
        m_record_sets[i].pipe_num = i;
        m_record_sets[i].records = m_records_from_sets[i];
    }
}
