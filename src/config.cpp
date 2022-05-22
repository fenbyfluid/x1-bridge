#include "config.h"
#include "defaults.h"

#include <nvs.h>

#include <stdexcept>

std::string Config::getName() {
    nvs_handle_t handle = ensureInitialized();

    size_t length = 0;
    esp_err_t err = nvs_get_str(handle, "name", nullptr, &length);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        throwError("nvs_get_str (len)", err);
    }

    if (length == 0) {
        return DEFAULT_NAME;
    }

    std::string value(length, '\0');
    err = nvs_get_str(handle, "name", &value[0], &length);
    if (err != ESP_OK) {
        throwError("nvs_get_str", err);
    }

    return value;
}

void Config::setName(const std::string &name) {
    nvs_handle_t handle = ensureInitialized();

    esp_err_t err = nvs_set_str(handle, "name", name.c_str());
    if (err != ESP_OK) {
        throwError("nvs_set_str", err);
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        throwError("nvs_commit", err);
    }
}

uint32_t Config::getPinCode() {
    nvs_handle_t handle = ensureInitialized();

    uint32_t value = 0;
    esp_err_t err = nvs_get_u32(handle, "pin-code", &value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        throwError("nvs_get_u32", err);
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return DEFAULT_PIN;
    }

    return value;
}

void Config::setPinCode(uint32_t pin_code) {
    nvs_handle_t handle = ensureInitialized();

    esp_err_t err = nvs_set_u32(handle, "pin-code", pin_code);
    if (err != ESP_OK) {
        throwError("nvs_set_str", err);
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        throwError("nvs_commit", err);
    }
}

std::optional<std::array<uint8_t, 6>> Config::getBtAddress() {
    nvs_handle_t handle = ensureInitialized();

    std::array<uint8_t, 6> value = {0};
    size_t length = value.size();
    esp_err_t err = nvs_get_blob(handle, "bt-address", value.data(), &length);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        throwError("nvs_get_blob", err);
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return std::nullopt;
    }

    return std::make_optional(value);
}

void Config::setBtAddress(const std::optional<std::array<uint8_t, 6>> &bt_address) {
    nvs_handle_t handle = ensureInitialized();

    esp_err_t err;
    if (bt_address) {
        err = nvs_set_blob(handle, "bt-address", bt_address->data(), bt_address->size());
        if (err != ESP_OK) {
            throwError("nvs_set_blob", err);
        }
    } else {
        err = nvs_erase_key(handle, "bt-address");
        if (err != ESP_OK) {
            throwError("nvs_erase_key", err);
        }
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        throwError("nvs_commit", err);
    }
}

void Config::reset() {
    nvs_handle_t handle = ensureInitialized();

    esp_err_t err = nvs_erase_all(handle);
    if (err != ESP_OK) {
        throwError("nvs_erase_all", err);
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        throwError("nvs_commit", err);
    }
}

nvs_handle_t Config::ensureInitialized() {
    static nvs_handle_t handle = 0;
    if (handle != 0) {
        return handle;
    }

    esp_err_t err = nvs_open("bridge-config", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        throwError("nvs_open", err);
    }

    return handle;
}

void Config::throwError(const std::string &label, esp_err_t err) {
    const char *name = esp_err_to_name(err);

    throw std::runtime_error(label + ": " + name);
}
