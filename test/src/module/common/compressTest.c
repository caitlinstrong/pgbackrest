/***********************************************************************************************************************************
Test Compression
***********************************************************************************************************************************/
#include "common/io/bufferRead.h"
#include "common/io/bufferWrite.h"
#include "common/io/filter/group.h"
#include "common/io/io.h"
#include "storage/posix/storage.h"

/***********************************************************************************************************************************
Compress data
***********************************************************************************************************************************/
static Buffer *
testCompress(IoFilter *compress, Buffer *decompressed, size_t inputSize, size_t outputSize)
{
    Buffer *compressed = bufNew(1024 * 1024);
    size_t inputTotal = 0;
    ioBufferSizeSet(outputSize);

    IoWrite *write = ioBufferWriteNew(compressed);
    ioFilterGroupAdd(ioWriteFilterGroup(write), compress);
    ioWriteOpen(write);

    // Compress input data
    while (inputTotal < bufSize(decompressed))
    {
        // Generate the input buffer based on input size. This breaks the data up into chunks as it would be in a real scenario.
        Buffer *input = bufNewC(
            bufPtr(decompressed) + inputTotal,
            inputSize > bufSize(decompressed) - inputTotal ? bufSize(decompressed) - inputTotal : inputSize);

        ioWrite(write, input);

        inputTotal += bufUsed(input);
        bufFree(input);
    }

    ioWriteClose(write);
    ioFilterFree(compress);

    return compressed;
}

/***********************************************************************************************************************************
Decompress data
***********************************************************************************************************************************/
static Buffer *
testDecompress(IoFilter *decompress, Buffer *compressed, size_t inputSize, size_t outputSize)
{
    Buffer *decompressed = bufNew(1024 * 1024);
    Buffer *output = bufNew(outputSize);
    ioBufferSizeSet(inputSize);

    IoRead *read = ioBufferReadNew(compressed);
    ioFilterGroupAdd(ioReadFilterGroup(read), decompress);
    ioReadOpen(read);

    while (!ioReadEof(read))
    {
        ioRead(read, output);
        bufCat(decompressed, output);
        bufUsedZero(output);
    }

    ioReadClose(read);
    bufFree(output);
    ioFilterFree(decompress);

    return decompressed;
}

/***********************************************************************************************************************************
Standard test suite to be applied to all compression types
***********************************************************************************************************************************/
static void
testSuite(CompressType type, const char *decompressCmd, size_t rawDelta)
{
    const char *simpleData = "A simple string";
    Buffer *compressed = NULL;
    Buffer *compressedRaw = NULL;
    Buffer *decompressed = bufNewC(simpleData, strlen(simpleData));

    PackWrite *packWrite = pckWriteNewP();
    pckWriteI32P(packWrite, 1);
    pckWriteBoolP(packWrite, false);
    pckWriteEndP(packWrite);

    // Create default storage object for testing
    Storage *storageTest = storagePosixNewP(TEST_PATH_STR, .write = true);

    TEST_TITLE("simple data");

    TEST_ASSIGN(
        compressed,
        testCompress(
            compressFilterPack(compressHelperLocal[type].compressType, pckWriteResult(packWrite)), decompressed, 1024,
            256 * 1024 * 1024),
        "simple data - compress large in/large out buffer");

    packWrite = pckWriteNewP();
    pckWriteI32P(packWrite, 1);
    pckWriteBoolP(packWrite, true);
    pckWriteEndP(packWrite);

    TEST_ASSIGN(
        compressedRaw,
        testCompress(
            compressFilterPack(compressHelperLocal[type].compressType, pckWriteResult(packWrite)), decompressed, 1024,
            1024),
        "simple data - compress large in/large out buffer (raw)");

    TEST_RESULT_UINT(bufUsed(compressed) - rawDelta, bufUsed(compressedRaw), "compare to raw");

    // -------------------------------------------------------------------------------------------------------------------------
    TEST_TITLE("compressed output can be decompressed with command-line tool");

    storagePutP(storageNewWriteP(storageTest, STRDEF("test.cmp")), compressed);
    HRN_SYSTEM_FMT("%s " TEST_PATH "/test.cmp > " TEST_PATH "/test.out 2> /dev/null", decompressCmd);
    TEST_RESULT_BOOL(bufEq(decompressed, storageGetP(storageNewReadP(storageTest, STRDEF("test.out")))), true, "check output");

    TEST_RESULT_BOOL(
        bufEq(compressed, testCompress(compressFilterP(type, 1), decompressed, 1024, 1)), true,
        "simple data - compress large in/small out buffer");

    TEST_RESULT_BOOL(
        bufEq(compressed, testCompress(compressFilterP(type, 1), decompressed, 1, 1024)), true,
        "simple data - compress small in/large out buffer");

    TEST_RESULT_BOOL(
        bufEq(compressed, testCompress(compressFilterP(type, 1), decompressed, 1, 1)), true,
        "simple data - compress small in/small out buffer");

    TEST_RESULT_BOOL(
        bufEq(compressedRaw, testCompress(compressFilterP(type, 1, .raw = true), decompressed, 1, 1)), true,
        "simple data - compress small in/small out buffer (raw)");

    packWrite = pckWriteNewP();
    pckWriteBoolP(packWrite, false);
    pckWriteEndP(packWrite);

    TEST_RESULT_BOOL(
        bufEq(
            decompressed,
            testDecompress(
                compressFilterPack(compressHelperLocal[type].decompressType, pckWriteResult(packWrite)), compressed, 1024, 1024)),
        true, "simple data - decompress large in/small out buffer");

    packWrite = pckWriteNewP();
    pckWriteBoolP(packWrite, true);
    pckWriteEndP(packWrite);

    TEST_RESULT_BOOL(
        bufEq(
            decompressed,
            testDecompress(
                compressFilterPack(
                    compressHelperLocal[type].decompressType, pckWriteResult(packWrite)), compressedRaw, 1024, 1024)),
        true, "simple data - decompress large in/large out buffer (raw)");

    TEST_RESULT_BOOL(
        bufEq(decompressed, testDecompress(decompressFilterP(type), compressed, 1024, 1)), true,
        "simple data - decompress large in/small out buffer");

    TEST_RESULT_BOOL(
        bufEq(decompressed, testDecompress(decompressFilterP(type), compressed, 1, 1024)), true,
        "simple data - decompress small in/large out buffer");

    TEST_RESULT_BOOL(
        bufEq(decompressed, testDecompress(decompressFilterP(type), compressed, 1, 1)), true,
        "simple data - decompress small in/small out buffer");

    // -------------------------------------------------------------------------------------------------------------------------
    TEST_TITLE("error on no compression data");

    TEST_ERROR(testDecompress(decompressFilterP(type), bufNew(0), 1, 1), FormatError, "unexpected eof in compressed data");

    // -------------------------------------------------------------------------------------------------------------------------
    TEST_TITLE("error on truncated compression data");

    Buffer *truncated = bufNew(0);
    bufCatSub(truncated, compressed, 0, bufUsed(compressed) - 1);

    TEST_RESULT_UINT(bufUsed(truncated), bufUsed(compressed) - 1, "check truncated buffer size");
    TEST_ERROR(testDecompress(decompressFilterP(type), truncated, 512, 512), FormatError, "unexpected eof in compressed data");

    // -------------------------------------------------------------------------------------------------------------------------
    TEST_TITLE("compress a large non-zero input buffer into small output buffer");

    decompressed = bufNew(1024 * 1024 - 1);
    uint8_t *chr = bufPtr(decompressed);

    // Step through the buffer, setting the individual bytes in a simple pattern (visible ASCII characters, DEC 32 - 126), to make
    // sure that we fill the compression library's small output buffer
    for (size_t chrIdx = 0; chrIdx < bufSize(decompressed); chrIdx++)
        chr[chrIdx] = (uint8_t)(chrIdx % 94 + 32);

    bufUsedSet(decompressed, bufSize(decompressed));

    TEST_ASSIGN(
        compressed, testCompress(compressFilterP(type, 3), decompressed, bufSize(decompressed), 32),
        "non-zero data - compress large in/small out buffer");

    TEST_RESULT_BOOL(
        bufEq(decompressed, testDecompress(decompressFilterP(type), compressed, bufSize(compressed), 1024 * 256)), true,
        "non-zero data - decompress large in/small out buffer");
}

/***********************************************************************************************************************************
Test Run
***********************************************************************************************************************************/
static void
testRun(void)
{
    FUNCTION_HARNESS_VOID();

    // *****************************************************************************************************************************
    if (testBegin("gz"))
    {
        // Run standard test suite
        testSuite(compressTypeGz, "gzip -dc", 12);

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("gzError()");

        TEST_RESULT_INT(gzError(Z_OK), Z_OK, "check ok");
        TEST_RESULT_INT(gzError(Z_STREAM_END), Z_STREAM_END, "check stream end");
        TEST_ERROR(gzError(Z_NEED_DICT), AssertError, "zlib threw error: [2] need dictionary");
        TEST_ERROR(gzError(Z_ERRNO), AssertError, "zlib threw error: [-1] file error");
        TEST_ERROR(gzError(Z_STREAM_ERROR), FormatError, "zlib threw error: [-2] stream error");
        TEST_ERROR(gzError(Z_DATA_ERROR), FormatError, "zlib threw error: [-3] data error");
        TEST_ERROR(gzError(Z_MEM_ERROR), MemoryError, "zlib threw error: [-4] insufficient memory");
        TEST_ERROR(gzError(Z_BUF_ERROR), AssertError, "zlib threw error: [-5] no space in buffer");
        TEST_ERROR(gzError(Z_VERSION_ERROR), FormatError, "zlib threw error: [-6] incompatible version");
        TEST_ERROR(gzError(999), AssertError, "zlib threw error: [999] unknown error");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("gzDecompressToLog() and gzCompressToLog()");

        char buffer[STACK_TRACE_PARAM_MAX];

        GzDecompress *decompress = (GzDecompress *)ioFilterDriver(gzDecompressNew(false));

        TEST_RESULT_VOID(FUNCTION_LOG_OBJECT_FORMAT(decompress, gzDecompressToLog, buffer, sizeof(buffer)), "gzDecompressToLog");
        TEST_RESULT_Z(buffer, "{inputSame: false, done: false, availIn: 0}", "check log");

        decompress->inputSame = true;
        decompress->done = true;

        TEST_RESULT_VOID(FUNCTION_LOG_OBJECT_FORMAT(decompress, gzDecompressToLog, buffer, sizeof(buffer)), "gzDecompressToLog");
        TEST_RESULT_Z(buffer, "{inputSame: true, done: true, availIn: 0}", "check log");
    }

    // *****************************************************************************************************************************
    if (testBegin("bz2"))
    {
        // Run standard test suite
        testSuite(compressTypeBz2, "bzip2 -dc", 0);

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("bz2Error()");

        TEST_RESULT_INT(bz2Error(BZ_OK), BZ_OK, "check ok");
        TEST_RESULT_INT(bz2Error(BZ_RUN_OK), BZ_RUN_OK, "check run ok");
        TEST_RESULT_INT(bz2Error(BZ_FLUSH_OK), BZ_FLUSH_OK, "check flush ok");
        TEST_RESULT_INT(bz2Error(BZ_FINISH_OK), BZ_FINISH_OK, "check finish ok");
        TEST_RESULT_INT(bz2Error(BZ_STREAM_END), BZ_STREAM_END, "check stream end");
        TEST_ERROR(bz2Error(BZ_SEQUENCE_ERROR), AssertError, "bz2 error: [-1] sequence error");
        TEST_ERROR(bz2Error(BZ_PARAM_ERROR), AssertError, "bz2 error: [-2] parameter error");
        TEST_ERROR(bz2Error(BZ_MEM_ERROR), MemoryError, "bz2 error: [-3] memory error");
        TEST_ERROR(bz2Error(BZ_DATA_ERROR), FormatError, "bz2 error: [-4] data error");
        TEST_ERROR(bz2Error(BZ_DATA_ERROR_MAGIC), FormatError, "bz2 error: [-5] data error magic");
        TEST_ERROR(bz2Error(BZ_IO_ERROR), AssertError, "bz2 error: [-6] io error");
        TEST_ERROR(bz2Error(BZ_UNEXPECTED_EOF), AssertError, "bz2 error: [-7] unexpected eof");
        TEST_ERROR(bz2Error(BZ_OUTBUFF_FULL), AssertError, "bz2 error: [-8] outbuff full");
        TEST_ERROR(bz2Error(BZ_CONFIG_ERROR), AssertError, "bz2 error: [-9] config error");
        TEST_ERROR(bz2Error(-999), AssertError, "bz2 error: [-999] unknown error");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("bz2DecompressToLog() and bz2CompressToLog()");

        char buffer[STACK_TRACE_PARAM_MAX];

        Bz2Compress *compress = (Bz2Compress *)ioFilterDriver(bz2CompressNew(1, false));

        compress->stream.avail_in = 999;

        TEST_RESULT_VOID(FUNCTION_LOG_OBJECT_FORMAT(compress, bz2CompressToLog, buffer, sizeof(buffer)), "bz2CompressToLog");
        TEST_RESULT_Z(buffer, "{inputSame: false, done: false, flushing: false, avail_in: 999}", "check log");

        Bz2Decompress *decompress = (Bz2Decompress *)ioFilterDriver(bz2DecompressNew(false));

        decompress->inputSame = true;
        decompress->done = true;

        TEST_RESULT_VOID(FUNCTION_LOG_OBJECT_FORMAT(decompress, bz2DecompressToLog, buffer, sizeof(buffer)), "bz2DecompressToLog");
        TEST_RESULT_Z(buffer, "{inputSame: true, done: true, avail_in: 0}", "check log");
    }

    // *****************************************************************************************************************************
    if (testBegin("lz4"))
    {
        // Run standard test suite
        testSuite(compressTypeLz4, "lz4 -dc", 4);

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("lz4Error()");

        TEST_RESULT_UINT(lz4Error(0), 0, "check success");
        TEST_ERROR(lz4Error((size_t)-2), FormatError, "lz4 error: [-2] ERROR_maxBlockSize_invalid");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("lz4DecompressToLog() and lz4CompressToLog()");

        char buffer[STACK_TRACE_PARAM_MAX];

        Lz4Compress *compress = (Lz4Compress *)ioFilterDriver(lz4CompressNew(7, false));

        compress->inputSame = true;
        compress->flushing = true;

        TEST_RESULT_VOID(FUNCTION_LOG_OBJECT_FORMAT(compress, lz4CompressToLog, buffer, sizeof(buffer)), "lz4CompressToLog");
        TEST_RESULT_Z(buffer, "{level: 7, first: true, inputSame: true, flushing: true}", "check log");

        Lz4Decompress *decompress = (Lz4Decompress *)ioFilterDriver(lz4DecompressNew(false));

        decompress->inputSame = true;
        decompress->done = true;
        decompress->inputOffset = 999;

        TEST_RESULT_VOID(FUNCTION_LOG_OBJECT_FORMAT(decompress, lz4DecompressToLog, buffer, sizeof(buffer)), "lz4DecompressToLog");
        TEST_RESULT_Z(buffer, "{inputSame: true, inputOffset: 999, frameDone false, done: true}", "check log");
    }

    // *****************************************************************************************************************************
    if (testBegin("zst"))
    {
#ifdef HAVE_LIBZST
        // Run standard test suite
        testSuite(compressTypeZst, "zstd -dc", 0);

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("zstError()");

        TEST_RESULT_UINT(zstError(0), 0, "check success");
        TEST_ERROR(zstError((size_t)-12), FormatError, "zst error: [-12] Version not supported");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("zstDecompressToLog() and zstCompressToLog()");

        char buffer[STACK_TRACE_PARAM_MAX];

        ZstCompress *compress = (ZstCompress *)ioFilterDriver(zstCompressNew(14, false));

        compress->inputSame = true;
        compress->inputOffset = 49;
        compress->flushing = true;

        TEST_RESULT_VOID(FUNCTION_LOG_OBJECT_FORMAT(compress, zstCompressToLog, buffer, sizeof(buffer)), "zstCompressToLog");
        TEST_RESULT_Z(buffer, "{level: 14, inputSame: true, inputOffset: 49, flushing: true}", "check log");

        ZstDecompress *decompress = (ZstDecompress *)ioFilterDriver(zstDecompressNew(false));

        decompress->inputSame = true;
        decompress->done = true;
        decompress->inputOffset = 999;

        TEST_RESULT_VOID(FUNCTION_LOG_OBJECT_FORMAT(decompress, zstDecompressToLog, buffer, sizeof(buffer)), "zstDecompressToLog");
        TEST_RESULT_Z(buffer, "{inputSame: true, inputOffset: 999, frameDone false, done: true}", "check log");
#else
        TEST_ERROR(compressTypePresent(compressTypeZst), OptionInvalidValueError, "pgBackRest not built with zst support");
#endif // HAVE_LIBZST
    }

    // Test everything in the helper that is not tested in the individual compression type tests
    // *****************************************************************************************************************************
    if (testBegin("helper"))
    {
        TEST_TITLE("compressTypeEnum()");

        TEST_RESULT_UINT(compressTypeEnum(strIdFromZ("none")), compressTypeNone, "none enum");
        TEST_RESULT_UINT(compressTypeEnum(strIdFromZ("gz")), compressTypeGz, "gz enum");
        TEST_ERROR(compressTypeEnum(strIdFromZ(BOGUS_STR)), AssertError, "invalid compression type 'BOGUS'");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("compressTypeStr()");

        TEST_RESULT_STR_Z(compressTypeStr(compressTypeGz), "gz", "gz str");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("compressTypePresent()");

        TEST_RESULT_VOID(compressTypePresent(compressTypeNone), "type none always present");
        TEST_ERROR(compressTypePresent(compressTypeXz), OptionInvalidValueError, "pgBackRest not built with xz support");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("compressTypeFromName()");

        TEST_RESULT_UINT(compressTypeFromName(STRDEF("file")), compressTypeNone, "type from name");
        TEST_RESULT_UINT(compressTypeFromName(STRDEF("file.gz")), compressTypeGz, "type from name");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("compressFilterPack()");

        TEST_RESULT_PTR(compressFilterPack(STRID5("bogus", 0x13a9de20), NULL), NULL, "no filter match");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("compressExtStr()");

        TEST_RESULT_STR_Z(compressExtStr(compressTypeNone), "", "one ext");
        TEST_RESULT_STR_Z(compressExtStr(compressTypeGz), ".gz", "gz ext");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("compressExtCat()");

        String *file = strCatZ(strNew(), "file");
        TEST_RESULT_VOID(compressExtCat(file, compressTypeGz), "cat gz ext");
        TEST_RESULT_STR_Z(file, "file.gz", "    check gz ext");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("compressExtStrip()");

        TEST_ERROR(compressExtStrip(STRDEF("file"), compressTypeGz), FormatError, "'file' must have '.gz' extension");
        TEST_RESULT_STR_Z(compressExtStrip(STRDEF("file"), compressTypeNone), "file", "nothing to strip");
        TEST_RESULT_STR_Z(compressExtStrip(STRDEF("file.gz"), compressTypeGz), "file", "strip gz");
    }

    FUNCTION_HARNESS_RETURN_VOID();
}
