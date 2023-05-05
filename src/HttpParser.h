/*
 * Authored by Alex Hultman, 2018-2020.
 * Intellectual property of third-party.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef UWS_HTTPPARSER_H
#define UWS_HTTPPARSER_H

// todo: HttpParser is in need of a few clean-ups and refactorings

/* The HTTP parser is an independent module subject to unit testing / fuzz testing */

#include <string>
#include <cstring>
#include <algorithm>
#include <climits>
#include "MoveOnlyFunction.h"
#include "ChunkedEncoding.h"

#include "BloomFilter.h"
#include "ProxyParser.h"
#include "QueryParser.h"

namespace uWS {

/* We require at least this much post padding */
static const unsigned int MINIMUM_HTTP_POST_PADDING = 32;
static void *FULLPTR = (void *)~(uintptr_t)0;

struct HttpRequest {

    friend struct HttpParser;

private:
    const static int MAX_HEADERS = 50;
    struct Header {
        std::string_view key, value;
    } headers[MAX_HEADERS];
    bool ancientHttp;
    unsigned int querySeparator;
    bool didYield;
    BloomFilter bf;
    std::pair<int, std::string_view *> currentParameters;

public:
    bool isAncient() {
        return ancientHttp;
    }

    bool getYield() {
        return didYield;
    }

    /* Iteration over headers (key, value) */
    struct HeaderIterator {
        Header *ptr;

        bool operator!=(const HeaderIterator &other) const {
            /* Comparison with end is a special case */
            if (ptr != other.ptr) {
                return other.ptr || ptr->key.length();
            }
            return false;
        }

        HeaderIterator &operator++() {
            ptr++;
            return *this;
        }

        std::pair<std::string_view, std::string_view> operator*() const {
            return {ptr->key, ptr->value};
        }
    };

    HeaderIterator begin() {
        return {headers + 1};
    }

    HeaderIterator end() {
        return {nullptr};
    }

    /* If you do not want to handle this route */
    void setYield(bool yield) {
        didYield = yield;
    }

    std::string_view getHeader(std::string_view lowerCasedHeader) {
        if (bf.mightHave(lowerCasedHeader)) {
            for (Header *h = headers; (++h)->key.length(); ) {
                if (h->key.length() == lowerCasedHeader.length() && !strncmp(h->key.data(), lowerCasedHeader.data(), lowerCasedHeader.length())) {
                    return h->value;
                }
            }
        }
        return std::string_view(nullptr, 0);
    }

    std::string_view getUrl() {
        return std::string_view(headers->value.data(), querySeparator);
    }

    std::string_view getFullUrl() {
        return std::string_view(headers->value.data(), headers->value.length());
    }

    /* Hack: this should be getMethod */
    std::string_view getCaseSensitiveMethod() {
        return std::string_view(headers->key.data(), headers->key.length());
    }

    std::string_view getMethod() {
        /* Compatibility hack: lower case method (todo: remove when major version bumps) */
        for (unsigned int i = 0; i < headers->key.length(); i++) {
            ((char *) headers->key.data())[i] |= 32;
        }

        return std::string_view(headers->key.data(), headers->key.length());
    }

    /* Returns the raw querystring as a whole, still encoded */
    std::string_view getQuery() {
        if (querySeparator < headers->value.length()) {
            /* Strip the initial ? */
            return std::string_view(headers->value.data() + querySeparator + 1, headers->value.length() - querySeparator - 1);
        } else {
            return std::string_view(nullptr, 0);
        }
    }

    /* Finds and decodes the URI component. */
    std::string_view getQuery(std::string_view key) {
        /* Raw querystring including initial '?' sign */
        std::string_view queryString = std::string_view(headers->value.data() + querySeparator, headers->value.length() - querySeparator);

        return getDecodedQueryValue(key, queryString);
    }

    void setParameters(std::pair<int, std::string_view *> parameters) {
        currentParameters = parameters;
    }

    std::string_view getParameter(unsigned short index) {
        if (currentParameters.first < (int) index) {
            return {};
        } else {
            return currentParameters.second[index];
        }
    }

};

struct HttpParser {

private:
    std::string fallback;
    /* This guy really has only 30 bits since we reserve two highest bits to chunked encoding parsing state */
    unsigned int remainingStreamingBytes = 0;

    const size_t MAX_FALLBACK_SIZE = 1024 * 4;

    /* Returns UINT_MAX on error. Maximum 999999999 is allowed. */
    static unsigned int toUnsignedInteger(std::string_view str) {
        /* We assume at least 32-bit integer giving us safely 999999999 (9 number of 9s) */
        if (str.length() > 9) {
            return UINT_MAX;
        }

        unsigned int unsignedIntegerValue = 0;
        for (char c : str) {
            /* As long as the letter is 0-9 we cannot overflow. */
            if (c < '0' || c > '9') {
                return UINT_MAX;
            }
            unsignedIntegerValue = unsignedIntegerValue * 10u + ((unsigned int) c - (unsigned int) '0');
        }
        return unsignedIntegerValue;
    }
   
    /* RFC 9110 16.3.1 Field Name Registry (TLDR; alnum + hyphen is allowed)
     * [...] It MUST conform to the field-name syntax defined in Section 5.1,
     * and it SHOULD be restricted to just letters, digits,
     * and hyphen ('-') characters, with the first character being a letter. */
    static inline bool isFieldNameByte(unsigned char x) {
        return (x == '-') |
        ((x > '/') & (x < ':')) |
        ((x > '@') & (x < '[')) |
        ((x > 96) & (x < '{'));
    }
    
    static void *memchr_r(const char *p, const char *end) {
        uint64_t mask = *(uint64_t *)"\r\r\r\r\r\r\r\r";
        if (p <= end - 8) {
            for (; p <= end - 8; p += 8) {
                uint64_t val = *(uint64_t *)p ^ mask;
                if ((val + 0xfefefefefefefeffull) & (~val & 0x8080808080808080ull)) {
                    break;
                }
            }
        }

        for (; p < end; p++) {
            if (*(unsigned char *)p == '\r') {
                return (void *)p;
            }
        }

        return nullptr;
     }

    static unsigned int getHeaders(char *postPaddedBuffer, char *end, struct HttpRequest::Header *headers, void *reserved) {
        char *preliminaryKey, *preliminaryValue, *start = postPaddedBuffer;

        #ifdef UWS_WITH_PROXY
            /* ProxyParser is passed as reserved parameter */
            ProxyParser *pp = (ProxyParser *) reserved;

            /* Parse PROXY protocol */
            auto [done, offset] = pp->parse({start, (size_t) (end - postPaddedBuffer)});
            if (!done) {
                /* We do not reset the ProxyParser (on filure) since it is tied to this
                * connection, which is really only supposed to ever get one PROXY frame
                * anyways. We do however allow multiple PROXY frames to be sent (overwrites former). */
                return 0;
            } else {
                /* We have consumed this data so skip it */
                start += offset;
            }
        #else
            /* This one is unused */
            (void) reserved;
            (void) end;
        #endif

        /* It is critical for fallback buffering logic that we only return with success
         * if we managed to parse a complete HTTP request (minus data). Returning success
         * for PROXY means we can end up succeeding, yet leaving bytes in the fallback buffer
         * which is then removed, and our counters to flip due to overflow and we end up with a crash */

        for (unsigned int i = 0; i < HttpRequest::MAX_HEADERS; i++) {
            for (preliminaryKey = postPaddedBuffer; (*postPaddedBuffer != ':') & (*(unsigned char *)postPaddedBuffer > 32); *(postPaddedBuffer++) |= 32);
            if (*postPaddedBuffer == '\r') {
                if ((postPaddedBuffer != end) & (postPaddedBuffer[1] == '\n') & (i > 0)) {
                    headers->key = std::string_view(nullptr, 0);
                    return (unsigned int) ((postPaddedBuffer + 2) - start);
                } else {
                    return 0;
                }
            } else {
                headers->key = std::string_view(preliminaryKey, (size_t) (postPaddedBuffer - preliminaryKey));
                for (postPaddedBuffer++; (*postPaddedBuffer == ':' || *(unsigned char *)postPaddedBuffer < 33) && *postPaddedBuffer != '\r'; postPaddedBuffer++);
                preliminaryValue = postPaddedBuffer;
                postPaddedBuffer = (char *) memchr_r(postPaddedBuffer, end);
                if (postPaddedBuffer && postPaddedBuffer[1] == '\n') {
                    headers->value = std::string_view(preliminaryValue, (size_t) (postPaddedBuffer - preliminaryValue));
                    postPaddedBuffer += 2;
                    headers++;
                } else {
                    return 0;
                }
            }
        }
        return 0;
    }

    /* This is the only caller of getHeaders and is thus the deepest part of the parser.
     * From here we return either [consumed, user] for "keep going",
      * or [consumed, nullptr] for "break; I am closed or upgraded to websocket"
      * or [whatever, fullptr] for "break and close me, I am a parser error!" */
    template <int CONSUME_MINIMALLY>
    std::pair<unsigned int, void *> fenceAndConsumePostPadded(char *data, unsigned int length, void *user, void *reserved, HttpRequest *req, MoveOnlyFunction<void *(void *, HttpRequest *)> &requestHandler, MoveOnlyFunction<void *(void *, std::string_view, bool)> &dataHandler) {

        /* How much data we CONSUMED (to throw away) */
        unsigned int consumedTotal = 0;

        /* Fence one byte past end of our buffer (buffer has post padded margins) */
        data[length] = '\r';

        for (unsigned int consumed; length && (consumed = getHeaders(data, data + length, req->headers, reserved)); ) {
            data += consumed;
            length -= consumed;
            consumedTotal += consumed;

            /* Store HTTP version (ancient 1.0 or 1.1) */
            req->ancientHttp = false;

            /* Add all headers to bloom filter */
            req->bf.reset();
            for (HttpRequest::Header *h = req->headers; (++h)->key.length(); ) {
                req->bf.add(h->key);
            }
            
            /* Break if no host header (but we can have empty string which is different from nullptr) */
            if (!req->getHeader("host").data()) {
                return {0, FULLPTR};
            }

            /* RFC 9112 6.3
            * If a message is received with both a Transfer-Encoding and a Content-Length header field,
            * the Transfer-Encoding overrides the Content-Length. Such a message might indicate an attempt
            * to perform request smuggling (Section 11.2) or response splitting (Section 11.1) and
            * ought to be handled as an error. */
            std::string_view transferEncodingString = req->getHeader("transfer-encoding");
            std::string_view contentLengthString = req->getHeader("content-length");
            if (transferEncodingString.length() && contentLengthString.length()) {
                /* Returning fullptr is the same as calling the errorHandler */
                /* We could be smart and set an error in the context along with this, to indicate what 
                 * http error response we might want to return */
                return {0, FULLPTR};
            }

            /* Parse query */
            const char *querySeparatorPtr = (const char *) memchr(req->headers->value.data(), '?', req->headers->value.length());
            req->querySeparator = (unsigned int) ((querySeparatorPtr ? querySeparatorPtr : req->headers->value.data() + req->headers->value.length()) - req->headers->value.data());

            /* If returned socket is not what we put in we need
             * to break here as we either have upgraded to
             * WebSockets or otherwise closed the socket. */
            void *returnedUser = requestHandler(user, req);
            if (returnedUser != user) {
                /* We are upgraded to WebSocket or otherwise broken */
                return {consumedTotal, returnedUser};
            }

            /* The rules at play here according to RFC 9112 for requests are essentially:
             * If both content-length and transfer-encoding then invalid message; must break.
             * If has transfer-encoding then must be chunked regardless of value.
             * If content-length then fixed length even if 0.
             * If none of the above then fixed length is 0. */

            /* RFC 9112 6.3
             * If a message is received with both a Transfer-Encoding and a Content-Length header field,
             * the Transfer-Encoding overrides the Content-Length. */
            if (transferEncodingString.length()) {

                /* If a proxy sent us the transfer-encoding header that 100% means it must be chunked or else the proxy is
                 * not RFC 9112 compliant. Therefore it is always better to assume this is the case, since that entirely eliminates 
                 * all forms of transfer-encoding obfuscation tricks. We just rely on the header. */

                /* RFC 9112 6.3
                 * If a Transfer-Encoding header field is present in a request and the chunked transfer coding is not the
                 * final encoding, the message body length cannot be determined reliably; the server MUST respond with the
                 * 400 (Bad Request) status code and then close the connection. */

                /* In this case we fail later by having the wrong interpretation (assuming chunked).
                 * This could be made stricter but makes no difference either way, unless forwarding the identical message as a proxy. */

                remainingStreamingBytes = STATE_IS_CHUNKED;
                /* If consume minimally, we do not want to consume anything but we want to mark this as being chunked */
                if (!CONSUME_MINIMALLY) {
                    /* Go ahead and parse it (todo: better heuristics for emitting FIN to the app level) */
                    std::string_view dataToConsume(data, length);
                    for (auto chunk : uWS::ChunkIterator(&dataToConsume, &remainingStreamingBytes)) {
                        dataHandler(user, chunk, chunk.length() == 0);
                    }
                    if (isParsingInvalidChunkedEncoding(remainingStreamingBytes)) {
                        return {0, FULLPTR};
                    }
                    unsigned int consumed = (length - (unsigned int) dataToConsume.length());
                    data = (char *) dataToConsume.data();
                    length = (unsigned int) dataToConsume.length();
                    consumedTotal += consumed;
                }
            } else if (contentLengthString.length()) {
                remainingStreamingBytes = toUnsignedInteger(contentLengthString);
                if (remainingStreamingBytes == UINT_MAX) {
                    /* Parser error */
                    return {0, FULLPTR};
                }

                if (!CONSUME_MINIMALLY) {
                    unsigned int emittable = std::min<unsigned int>(remainingStreamingBytes, length);
                    dataHandler(user, std::string_view(data, emittable), emittable == remainingStreamingBytes);
                    remainingStreamingBytes -= emittable;

                    data += emittable;
                    length -= emittable;
                    consumedTotal += emittable;
                }
            } else {
                /* If we came here without a body; emit an empty data chunk to signal no data */
                dataHandler(user, {}, true);
            }

            /* Consume minimally should break as easrly as possible */
            if (CONSUME_MINIMALLY) {
                break;
            }
        }
        return {consumedTotal, user};
    }

public:
    void *consumePostPadded(char *data, unsigned int length, void *user, void *reserved, MoveOnlyFunction<void *(void *, HttpRequest *)> &&requestHandler, MoveOnlyFunction<void *(void *, std::string_view, bool)> &&dataHandler, MoveOnlyFunction<void *(void *)> &&errorHandler) {

        /* This resets BloomFilter by construction, but later we also reset it again.
         * Optimize this to skip resetting twice (req could be made global) */
        HttpRequest req;

        if (remainingStreamingBytes) {

            /* It's either chunked or with a content-length */
            if (isParsingChunkedEncoding(remainingStreamingBytes)) {
                std::string_view dataToConsume(data, length);
                for (auto chunk : uWS::ChunkIterator(&dataToConsume, &remainingStreamingBytes)) {
                    dataHandler(user, chunk, chunk.length() == 0);
                }
                if (isParsingInvalidChunkedEncoding(remainingStreamingBytes)) {
                    return FULLPTR;
                }
                data = (char *) dataToConsume.data();
                length = (unsigned int) dataToConsume.length();
            } else {
                // this is exactly the same as below!
                // todo: refactor this
                if (remainingStreamingBytes >= length) {
                    void *returnedUser = dataHandler(user, std::string_view(data, length), remainingStreamingBytes == length);
                    remainingStreamingBytes -= length;
                    return returnedUser;
                } else {
                    void *returnedUser = dataHandler(user, std::string_view(data, remainingStreamingBytes), true);

                    data += remainingStreamingBytes;
                    length -= remainingStreamingBytes;

                    remainingStreamingBytes = 0;

                    if (returnedUser != user) {
                        return returnedUser;
                    }
                }
            }

        } else if (fallback.length()) {
            unsigned int had = (unsigned int) fallback.length();

            size_t maxCopyDistance = std::min<size_t>(MAX_FALLBACK_SIZE - fallback.length(), (size_t) length);

            /* We don't want fallback to be short string optimized, since we want to move it */
            fallback.reserve(fallback.length() + maxCopyDistance + std::max<unsigned int>(MINIMUM_HTTP_POST_PADDING, sizeof(std::string)));
            fallback.append(data, maxCopyDistance);

            // break here on break
            std::pair<unsigned int, void *> consumed = fenceAndConsumePostPadded<true>(fallback.data(), (unsigned int) fallback.length(), user, reserved, &req, requestHandler, dataHandler);
            if (consumed.second != user) {
                return consumed.second;
            }

            if (consumed.first) {

                /* This logic assumes that we consumed everything in fallback buffer.
                 * This is critically important, as we will get an integer overflow in case
                 * of "had" being larger than what we consumed, and that we would drop data */
                fallback.clear();
                data += consumed.first - had;
                length -= consumed.first - had;

                if (remainingStreamingBytes) {
                    /* It's either chunked or with a content-length */
                    if (isParsingChunkedEncoding(remainingStreamingBytes)) {
                        std::string_view dataToConsume(data, length);
                        for (auto chunk : uWS::ChunkIterator(&dataToConsume, &remainingStreamingBytes)) {
                            dataHandler(user, chunk, chunk.length() == 0);
                        }
                        if (isParsingInvalidChunkedEncoding(remainingStreamingBytes)) {
                            return FULLPTR;
                        }
                        data = (char *) dataToConsume.data();
                        length = (unsigned int) dataToConsume.length();
                    } else {
                        // this is exactly the same as above!
                        if (remainingStreamingBytes >= (unsigned int) length) {
                            void *returnedUser = dataHandler(user, std::string_view(data, length), remainingStreamingBytes == (unsigned int) length);
                            remainingStreamingBytes -= length;
                            return returnedUser;
                        } else {
                            void *returnedUser = dataHandler(user, std::string_view(data, remainingStreamingBytes), true);

                            data += remainingStreamingBytes;
                            length -= remainingStreamingBytes;

                            remainingStreamingBytes = 0;

                            if (returnedUser != user) {
                                return returnedUser;
                            }
                        }
                    }
                }

            } else {
                if (fallback.length() == MAX_FALLBACK_SIZE) {
                    // note: you don't really need error handler, just return something strange!
                    // we could have it return a constant pointer to denote error!
                    return errorHandler(user);
                }
                return user;
            }
        }

        std::pair<unsigned int, void *> consumed = fenceAndConsumePostPadded<false>(data, length, user, reserved, &req, requestHandler, dataHandler);
        if (consumed.second != user) {
            return consumed.second;
        }

        data += consumed.first;
        length -= consumed.first;

        if (length) {
            if (length < MAX_FALLBACK_SIZE) {
                fallback.append(data, length);
            } else {
                return errorHandler(user);
            }
        }

        // added for now
        return user;
    }
};

}

#endif // UWS_HTTPPARSER_H