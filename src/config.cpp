#include "config.h"
#include "defaults.h"

#include <nvs.h>

#include <stdexcept>

std::string Config::getName() {
    return getString("name").value_or(DEFAULT_NAME);
}

void Config::setName(const std::string &name) {
    setString("name", name);
}

uint32_t Config::getPinCode() {
    return getUint32("pin-code").value_or(DEFAULT_PIN);
}

void Config::setPinCode(uint32_t pin_code) {
    setUint32("pin-code", pin_code);
}

uint32_t Config::getConnectedIdleTimeout() {
    return getUint32("conn-timeout").value_or(DEFAULT_CONNECTED_IDLE_TIME);
}

void Config::setConnectedIdleTimeout(uint32_t timeout) {
    setUint32("conn-timeout", timeout);
}

uint32_t Config::getDisconnectedIdleTimeout() {
    return getUint32("disconn-timeout").value_or(DEFAULT_DISCONNECTED_IDLE_TIME);
}

void Config::setDisconnectedIdleTimeout(uint32_t timeout) {
    setUint32("disconn-timeout", timeout);
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

    return value;
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

std::optional<std::string> Config::getBtAddressName() {
    return getString("bt-addr-name");
}

void Config::setBtAddressName(const std::optional<std::string> &name) {
    setString("bt-addr-name", name);
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

std::optional<uint32_t> Config::getUint32(const char *key) {
    nvs_handle_t handle = ensureInitialized();

    uint32_t value = 0;
    esp_err_t err = nvs_get_u32(handle, key, &value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        throwError("nvs_get_u32", err);
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return std::nullopt;
    }

    return value;
}

void Config::setUint32(const char *key, const std::optional<uint32_t> &value) {
    nvs_handle_t handle = ensureInitialized();

    esp_err_t err;
    if (value) {
        err = nvs_set_u32(handle, key, *value);
        if (err != ESP_OK) {
            throwError("nvs_set_u32", err);
        }
    } else {
        err = nvs_erase_key(handle, key);
        if (err != ESP_OK) {
            throwError("nvs_erase_key", err);
        }
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        throwError("nvs_commit", err);
    }
}

std::optional<std::string> Config::getString(const char *key) {
    nvs_handle_t handle = ensureInitialized();

    size_t length = 0;
    esp_err_t err = nvs_get_str(handle, key, nullptr, &length);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        throwError("nvs_get_str (len)", err);
    }

    if (length == 0) {
        return std::nullopt;
    }

    std::string value(length, '\0');
    err = nvs_get_str(handle, key, &value[0], &length);
    if (err != ESP_OK) {
        throwError("nvs_get_str", err);
    }

    // Trim the extra terminator written by nvs_get_str.
    value.resize(length - 1);

    return value;
}

void Config::setString(const char *key, const std::optional<std::string> &value) {
    nvs_handle_t handle = ensureInitialized();

    esp_err_t err;
    if (value) {
        err = nvs_set_str(handle, key, value->c_str());
        if (err != ESP_OK) {
            throwError("nvs_set_str", err);
        }
    } else {
        err = nvs_erase_key(handle, key);
        if (err != ESP_OK) {
            throwError("nvs_erase_key", err);
        }
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        throwError("nvs_commit", err);
    }
}
