#pragma once
#include <cstdint>
#include <cstring>
#include "daisy_versio.h"
#include "slice_engine.h"

/**
 * QSPI Flash Persistence for Slot 0 (index 0)
 *
 * Layout in 8MB QSPI flash:
 *   [0x000000 - 0x000FFF] 4KB header sector: magic, settings, sample count
 *   [0x001000 - ...]       L channel float data (up to 3.6MB)
 *   [L_end   - ...]        R channel float data (up to 3.6MB)
 *   Max total: ~6.87MB + 4KB header = fits in 8MB
 *
 * Save trigger: record-stop on slot 0
 * Load trigger: boot
 *
 * Save is chunked across main-loop ticks so LEDs stay responsive.
 * Audio DMA continues uninterrupted — QSPI and SDRAM are on
 * separate buses, zero contention.
 *
 * Access via DaisyVersio: hw.seed.qspi
 */

// QSPI flash layout
static constexpr uint32_t QSPI_HEADER_ADDR    = 0x000000;
static constexpr uint32_t QSPI_HEADER_SIZE    = 4096;      // 1 sector (4KB)
static constexpr uint32_t QSPI_AUDIO_START    = 0x001000;  // right after header
static constexpr uint32_t QSPI_SECTOR_64K     = 0x10000;   // 64KB erase block
static constexpr uint32_t QSPI_TOTAL_SIZE     = 0x800000;  // 8MB

// Write chunk size per Tick() — 4KB keeps main loop responsive
// ~1ms per chunk on QSPI, so LEDs update smoothly
static constexpr uint32_t WRITE_CHUNK_BYTES   = 4096;

// Magic number to validate stored data
static constexpr uint32_t PERSIST_MAGIC        = 0x48534C43; // "HSLC"
static constexpr uint32_t PERSIST_VERSION      = 1;

/** Header stored at QSPI offset 0. Must be < 4096 bytes. */
struct PersistHeader
{
    uint32_t magic;           // PERSIST_MAGIC
    uint32_t version;         // PERSIST_VERSION
    uint32_t total_samples;   // number of audio samples stored
    uint32_t num_slices;      // slice count at time of save
    uint32_t bars;            // bar setting
    float    bpm;             // BPM setting
    uint32_t slices_setting;  // slice count setting
    float    sample_rate;     // sample rate at save time
    uint32_t crc32;           // simple CRC over audio data
    uint32_t reserved[7];     // future use, zero-filled
};
static_assert(sizeof(PersistHeader) <= QSPI_HEADER_SIZE, "Header too large");

enum class PersistState
{
    IDLE,
    ERASE,          // Erasing required sectors
    WRITE_L,        // Writing L channel in chunks
    WRITE_R,        // Writing R channel in chunks
    WRITE_HEADER,   // Writing header last (validates save)
    SAVE_DONE,      // Brief LED feedback state
    LOAD_PENDING,   // Set at boot, processed on first Tick
};

class PersistenceManager
{
  public:
    PersistenceManager() {}

    void Init(daisy::QSPIHandle* qspi_handle, SliceEngine* engine,
              float* buf_l, float* buf_r, uint32_t max_slot_samples)
    {
        qspi_     = qspi_handle;
        engine_   = engine;
        buf_l_    = buf_l;
        buf_r_    = buf_r;
        max_slot_ = max_slot_samples;
        state_    = PersistState::IDLE;
        save_done_timer_ = 0;
    }

    /** Call once at boot, before audio starts. Loads slot 0 if valid. */
    bool LoadOnBoot()
    {
        if(!qspi_) return false;

        // Read header from memory-mapped QSPI
        const uint8_t* flash = reinterpret_cast<const uint8_t*>(
            qspi_->GetData(QSPI_HEADER_ADDR));
        if(!flash) return false;

        PersistHeader hdr;
        memcpy(&hdr, flash, sizeof(PersistHeader));

        // Validate
        if(hdr.magic != PERSIST_MAGIC) return false;
        if(hdr.version != PERSIST_VERSION) return false;
        if(hdr.total_samples == 0) return false;
        if(hdr.total_samples > max_slot_) return false;

        uint32_t audio_bytes = hdr.total_samples * sizeof(float);

        // Verify CRC
        const uint8_t* audio_l_src = reinterpret_cast<const uint8_t*>(
            qspi_->GetData(QSPI_AUDIO_START));
        const uint8_t* audio_r_src = audio_l_src + audio_bytes;

        uint32_t check = ComputeCRC(audio_l_src, audio_bytes);
        check = ComputeCRC(audio_r_src, audio_bytes, check);
        if(check != hdr.crc32) return false;

        // Copy audio data from QSPI into SDRAM slot 0
        memcpy(buf_l_, audio_l_src, audio_bytes);
        memcpy(buf_r_, audio_r_src, audio_bytes);

        // Restore slot 0 header in engine
        engine_->RestoreSlot0(hdr.total_samples, hdr.num_slices,
                              hdr.sample_rate);

        // Restore settings
        loaded_settings_.bars   = hdr.bars;
        loaded_settings_.bpm    = hdr.bpm;
        loaded_settings_.slices = hdr.slices_setting;
        has_loaded_settings_    = true;

        return true;
    }

    /** Start saving slot 0. Call from main loop when recording stops. */
    void StartSave()
    {
        if(!qspi_ || !engine_) return;
        if(state_ != PersistState::IDLE) return;

        const SlotHeader& hdr = engine_->GetSlot0Header();
        if(!hdr.has_content || hdr.total_samples == 0) return;

        save_samples_ = hdr.total_samples;
        save_slices_  = hdr.num_slices;
        save_sr_      = hdr.sample_rate;

        // Calculate total bytes to write
        uint32_t audio_bytes = save_samples_ * sizeof(float);
        uint32_t total_data  = QSPI_HEADER_SIZE + (audio_bytes * 2);

        // How many 64K sectors need erasing
        erase_end_ = ((total_data + QSPI_SECTOR_64K - 1)
                       / QSPI_SECTOR_64K) * QSPI_SECTOR_64K;

        write_offset_ = 0;
        write_total_  = audio_bytes;
        state_ = PersistState::ERASE;
    }

    /** Call every main-loop iteration. Processes one chunk of work.
     *  Returns true while a save is in progress. */
    bool Tick()
    {
        switch(state_)
        {
            case PersistState::IDLE:
                return false;

            case PersistState::ERASE:
                DoErase();
                return true;

            case PersistState::WRITE_L:
                DoWriteL();
                return true;

            case PersistState::WRITE_R:
                DoWriteR();
                return true;

            case PersistState::WRITE_HEADER:
                DoWriteHeader();
                return true;

            case PersistState::SAVE_DONE:
                if(save_done_timer_ > 0)
                {
                    save_done_timer_--;
                    return true;
                }
                state_ = PersistState::IDLE;
                return false;

            default:
                return false;
        }
    }

    bool IsSaving() const { return state_ != PersistState::IDLE; }
    bool IsSaveDone() const { return state_ == PersistState::SAVE_DONE; }

    /** Progress 0.0-1.0 for LED feedback during save */
    float GetSaveProgress() const
    {
        if(write_total_ == 0) return 0.0f;
        uint32_t total_work = write_total_ * 2; // L + R
        uint32_t done = 0;
        switch(state_)
        {
            case PersistState::WRITE_L:
                done = write_offset_;
                break;
            case PersistState::WRITE_R:
                done = write_total_ + write_offset_;
                break;
            case PersistState::WRITE_HEADER:
            case PersistState::SAVE_DONE:
                done = total_work;
                break;
            default:
                break;
        }
        return static_cast<float>(done)
               / static_cast<float>(total_work);
    }

    bool HasLoadedSettings() const { return has_loaded_settings_; }
    RecordSettings GetLoadedSettings()
    {
        has_loaded_settings_ = false;
        return loaded_settings_;
    }

  private:
    void DoErase()
    {
        // Erase the needed range in one call.
        // This blocks for a few seconds for large recordings,
        // but audio DMA continues (separate bus).
        auto result = qspi_->Erase(0, erase_end_);
        if(result == daisy::QSPIHandle::Result::OK)
        {
            write_offset_ = 0;
            state_ = PersistState::WRITE_L;
        }
        else
        {
            // Erase failed — abort
            state_ = PersistState::IDLE;
        }
    }

    void DoWriteL()
    {
        uint32_t remaining = write_total_ - write_offset_;
        uint32_t chunk = (remaining > WRITE_CHUNK_BYTES)
                         ? WRITE_CHUNK_BYTES : remaining;

        uint32_t flash_addr = QSPI_AUDIO_START + write_offset_;
        uint8_t* src = reinterpret_cast<uint8_t*>(buf_l_) + write_offset_;

        auto result = qspi_->Write(flash_addr, chunk, src);
        if(result != daisy::QSPIHandle::Result::OK)
        {
            state_ = PersistState::IDLE; // abort
            return;
        }

        write_offset_ += chunk;
        if(write_offset_ >= write_total_)
        {
            write_offset_ = 0;
            state_ = PersistState::WRITE_R;
        }
    }

    void DoWriteR()
    {
        uint32_t remaining = write_total_ - write_offset_;
        uint32_t chunk = (remaining > WRITE_CHUNK_BYTES)
                         ? WRITE_CHUNK_BYTES : remaining;

        // R channel starts right after L channel in flash
        uint32_t flash_addr = QSPI_AUDIO_START + write_total_ + write_offset_;
        uint8_t* src = reinterpret_cast<uint8_t*>(buf_r_) + write_offset_;

        auto result = qspi_->Write(flash_addr, chunk, src);
        if(result != daisy::QSPIHandle::Result::OK)
        {
            state_ = PersistState::IDLE;
            return;
        }

        write_offset_ += chunk;
        if(write_offset_ >= write_total_)
        {
            state_ = PersistState::WRITE_HEADER;
        }
    }

    void DoWriteHeader()
    {
        // Compute CRC over what we just wrote to verify integrity
        uint32_t audio_bytes = save_samples_ * sizeof(float);
        const uint8_t* flash_l = reinterpret_cast<const uint8_t*>(
            qspi_->GetData(QSPI_AUDIO_START));
        const uint8_t* flash_r = flash_l + audio_bytes;

        uint32_t crc = ComputeCRC(flash_l, audio_bytes);
        crc = ComputeCRC(flash_r, audio_bytes, crc);

        // Build header
        PersistHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic          = PERSIST_MAGIC;
        hdr.version        = PERSIST_VERSION;
        hdr.total_samples  = save_samples_;
        hdr.num_slices     = save_slices_;
        hdr.bars           = engine_->GetSettings().bars;
        hdr.bpm            = engine_->GetSettings().bpm;
        hdr.slices_setting = engine_->GetSettings().slices;
        hdr.sample_rate    = save_sr_;
        hdr.crc32          = crc;

        // Header sector was already erased. Write it.
        auto result = qspi_->Write(QSPI_HEADER_ADDR,
                                   sizeof(PersistHeader),
                                   reinterpret_cast<uint8_t*>(&hdr));
        if(result == daisy::QSPIHandle::Result::OK)
        {
            save_done_timer_ = 200; // ~200 main loop ticks for LED flash
            state_ = PersistState::SAVE_DONE;
        }
        else
        {
            state_ = PersistState::IDLE;
        }
    }

    /** Simple CRC32 (polynomial 0xEDB88320, standard Ethernet CRC).
     *  Good enough for data integrity check. Not crypto. */
    static uint32_t ComputeCRC(const uint8_t* data, uint32_t len,
                               uint32_t crc = 0xFFFFFFFF)
    {
        for(uint32_t i = 0; i < len; i++)
        {
            crc ^= data[i];
            for(int b = 0; b < 8; b++)
            {
                if(crc & 1)
                    crc = (crc >> 1) ^ 0xEDB88320;
                else
                    crc >>= 1;
            }
        }
        return crc;
    }

    daisy::QSPIHandle* qspi_     = nullptr;
    SliceEngine*       engine_   = nullptr;
    float*             buf_l_    = nullptr;
    float*             buf_r_    = nullptr;
    uint32_t           max_slot_ = 0;

    PersistState state_ = PersistState::IDLE;

    // Save state
    uint32_t save_samples_    = 0;
    uint32_t save_slices_     = 0;
    float    save_sr_         = 0.0f;
    uint32_t erase_end_       = 0;
    uint32_t write_offset_    = 0;
    uint32_t write_total_     = 0;
    uint32_t save_done_timer_ = 0;

    // Load state
    RecordSettings loaded_settings_;
    bool has_loaded_settings_ = false;
};
