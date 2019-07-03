#include "encryption.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <string>
#include <sstream>

#define DATA_COPY_LENGTH 32
#define HALF_DATA_COPY_LENGTH 16

/**
 * 데이터 swap
 */
void char_swap(char* a, char* b) {
    char temp = *a;
    *a = *b;
    *b = temp;
}

/**
 * 입력 데이터를 키 값으로 암호화
 * 입력 데이터를 키 값과 xor 연산을 수행한 후 순서를 섞음
 */
void encrypt(char* str, int strLength, const char* key, int keyLength) {
    for (int i = 0; i < strLength; i++) {
        str[i] ^= key[i%keyLength];
    }
    for (int i = 0; i < strLength / 2; i++) {
        char_swap(&str[i], &str[strLength - 1 - i]);
    }
}

/**
 * 암호화된 데이터를 키 값으로 복호화
 * 암호화의 역순으로 복호화 진행
 */
void decrypt(char* str, int strLength, const char* key, int keyLength) {
    for (int i = 0; i < strLength / 2; i++) {
        char_swap(&str[i], &str[strLength - 1 - i]);
    }
    for (int i = 0; i < strLength; i++) {
        str[i] ^= key[i%keyLength];
    }
}

/*
 * [Data Copy] (32 byte)
 * ["Selvas"] (6 Byte)
 * [Data Length] (4 byte)
 * [Data] (n Byte)
 */
int addEncryptHeader(const char* input, char** output, int byteLength) {
    int size = DATA_COPY_LENGTH + 6 + 4 + byteLength + 1;

    *output = (char*)malloc(sizeof(char)*size);

    char* temp = *output;
    memset(temp, 0x00, sizeof(char)*size);

    int data_offset = 0;
    if (byteLength >= DATA_COPY_LENGTH) {
        memcpy(temp + data_offset, input, sizeof(char)*HALF_DATA_COPY_LENGTH);
        data_offset += (HALF_DATA_COPY_LENGTH);
        memcpy(temp + data_offset, input + byteLength - (HALF_DATA_COPY_LENGTH), sizeof(char)*HALF_DATA_COPY_LENGTH);
        data_offset += (HALF_DATA_COPY_LENGTH);
    }
    else {
        memcpy(temp + data_offset, input, sizeof(char)*byteLength);
        data_offset += (DATA_COPY_LENGTH);
    }

    temp[data_offset++] = 'S';
    temp[data_offset++] = 'e';
    temp[data_offset++] = 'l';
    temp[data_offset++] = 'v';
    temp[data_offset++] = 'a';
    temp[data_offset++] = 's';
    temp[data_offset++] = ((byteLength & 0xFF000000) >> 24) & 0xFF;
    temp[data_offset++] = ((byteLength & 0x00FF0000) >> 16) & 0xFF;
    temp[data_offset++] = ((byteLength & 0x0000FF00) >> 8) & 0xFF;
    temp[data_offset++] = ((byteLength & 0x000000FF)) & 0xFF;

    memcpy(temp + data_offset, input, sizeof(char)*byteLength);

    return size;
}

std::string encrypt_image(const std::string& image, const std::string& keyStr) {
    if (image.empty() || keyStr.empty()) {
        return image;
    }

    const auto input = image.data();
    const auto key = keyStr.data();

    auto temp = new char[image.size() + 1];
    memcpy(temp, input, sizeof(char)*(image.size() + 1));

    encrypt(temp, image.size(), key, keyStr.size());

    std::string result(temp, temp + image.size());
    delete[] temp;

    return result;
}

std::string decrypt_image(const std::string& image, const std::string& keyStr) {
    if (image.empty() || keyStr.empty()) {
        return image;
    }

    const auto input = image.data();
    const auto key = keyStr.data();

    auto temp = new char[image.size() + 1];
    memcpy(temp, input, sizeof(char)*(image.size() + 1));

    decrypt(temp, image.size(), key, keyStr.size());

    std::string result(temp, temp + image.size());
    delete[] temp;
    return result;
}

std::string encrypt_image_internal_key(const std::string& image) {
    const auto input = image.data();

    char* addHeader;
    int size = addEncryptHeader(input, &addHeader, image.size());

    char key[28] = { 'J' ^ 'S',
        'x' ^ 'e',
        'l' ^ 'l',
        'u' ^ 'v',
        'O' ^ 'a',
        'G' ^ 's',
        'u' ^ '-',
        'H' ^ 'A',
        'X' ^ 'I',
        'n' ^ '/',
        'a' ^ '1',
        't' ^ 'D',
        '9' ^ 'C',
        'w' ^ 'a',
        'D' ^ 'r',
        'n' ^ 'd',
        '6' ^ '/',
        'o' ^ 'R',
        'T' ^ 'e',
        'k' ^ 'c',
        'x' ^ 'o',
        'u' ^ 'g',
        '7' ^ 'n',
        'j' ^ '1',
        'P' ^ '2',
        'f' ^ 'e',
        'o' ^ 'r',
        '=' ^ ';' };
    int keyLength = 28;

    encrypt(addHeader, size, key, keyLength);
    std::string result(addHeader, addHeader + size);
    free(addHeader);

    return result;
}

std::string decrypt_image_internal_key(const std::string& image) {
    const auto minLength = (DATA_COPY_LENGTH + 6 + 4 + 1);
    if (image.empty() || image.size() < minLength) {
        return std::string();
    }

    const auto input = image.data();

    auto temp = static_cast<char *>(malloc(sizeof(char) * (image.size() + 1)));
    memcpy(temp, input, sizeof(char)*(image.size() + 1));

    const char key[28] = { 'J' ^ 'S',
        'x' ^ 'e',
        'l' ^ 'l',
        'u' ^ 'v',
        'O' ^ 'a',
        'G' ^ 's',
        'u' ^ '-',
        'H' ^ 'A',
        'X' ^ 'I',
        'n' ^ '/',
        'a' ^ '1',
        't' ^ 'D',
        '9' ^ 'C',
        'w' ^ 'a',
        'D' ^ 'r',
        'n' ^ 'd',
        '6' ^ '/',
        'o' ^ 'R',
        'T' ^ 'e',
        'k' ^ 'c',
        'x' ^ 'o',
        'u' ^ 'g',
        '7' ^ 'n',
        'j' ^ '1',
        'P' ^ '2',
        'f' ^ 'e',
        'o' ^ 'r',
        '=' ^ ';' };
    const auto keyLength = 28;

    decrypt(temp, image.size(), key, keyLength);

    // check selvas
    auto offset = DATA_COPY_LENGTH;
    if (temp[offset + 0] != 'S'
        || temp[offset + 1] != 'e'
        || temp[offset + 2] != 'l'
        || temp[offset + 3] != 'v'
        || temp[offset + 4] != 'a'
        || temp[offset + 5] != 's') {
        free(temp);
        return std::string();
    }

    offset += 6;
    auto data_length = 0;
    data_length = ((temp[offset + 0] & 0xFF) << 24) |
        ((temp[offset + 1] & 0xFF) << 16) |
        ((temp[offset + 2] & 0xFF) << 8) |
        (temp[offset + 3] & 0xFF);
    offset += 4;
    if (image.size() < offset + data_length) {
        free(temp);
        return std::string();
    }

    if (data_length >= DATA_COPY_LENGTH) {
        for (auto i = 0; i < HALF_DATA_COPY_LENGTH; i++) {
            if ((temp[i] != temp[offset + i])
                || (temp[HALF_DATA_COPY_LENGTH + i] != temp[offset + data_length - HALF_DATA_COPY_LENGTH + i])) {
                free(temp);
                return std::string();
            }
        }
    }
    else {
        for (auto i = 0; i < data_length; i++) {
            if ((temp[i] != temp[offset + i])) {
                free(temp);
                return std::string();
            }
        }
    }

    std::string result(temp + offset, temp + offset + data_length);
    free(temp);
    return result;
}

std::string encrypt_text(const std::string& textStr, const std::string& keyStr) {
    if (textStr.empty() || keyStr.empty()) {
        return textStr;
    }

    const auto text = textStr.data();
    const auto key = keyStr.data();
    auto temp = new char[textStr.size() + 1];
    memcpy(temp, text, sizeof(char)*(textStr.size() + 1));

    encrypt(temp, textStr.size(), key, keyStr.size());
    std::string result(temp, temp + textStr.size());
    delete[] temp;

    return result;
}

std::string decrypt_text(const std::string& textStr, const std::string& keyStr) {
    const auto text = textStr.data();
    const auto key = keyStr.data();

    if (textStr.empty() || keyStr.empty()) {
        return textStr;
    }

    auto temp = new char[textStr.size() + 1];
    memcpy(temp, text, sizeof(char)*(textStr.size() + 1));

    decrypt(temp, textStr.size(), key, keyStr.size());
    std::string result(temp, temp + textStr.size());

    delete[] temp;

    return result;
}

std::string generate_key()
{
    std::stringstream ss;
    ss << "1selvasai@";
    return ss.str();
}

