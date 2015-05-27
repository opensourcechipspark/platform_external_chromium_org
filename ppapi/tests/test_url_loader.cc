// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ppapi/tests/test_url_loader.h"

#include <stdio.h>
#include <string.h>
#include <string>

#include "ppapi/c/pp_errors.h"
#include "ppapi/c/ppb_file_io.h"
#include "ppapi/c/ppb_url_loader.h"
#include "ppapi/c/trusted/ppb_url_loader_trusted.h"
#include "ppapi/cpp/dev/url_util_dev.h"
#include "ppapi/cpp/file_io.h"
#include "ppapi/cpp/file_ref.h"
#include "ppapi/cpp/file_system.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/private/file_io_private.h"
#include "ppapi/cpp/url_loader.h"
#include "ppapi/cpp/url_request_info.h"
#include "ppapi/cpp/url_response_info.h"
#include "ppapi/tests/test_utils.h"
#include "ppapi/tests/testing_instance.h"

REGISTER_TEST_CASE(URLLoader);

namespace {

int32_t WriteEntireBuffer(PP_Instance instance,
                          pp::FileIO* file_io,
                          int32_t offset,
                          const std::string& data,
                          CallbackType callback_type) {
  TestCompletionCallback callback(instance, callback_type);
  int32_t write_offset = offset;
  const char* buf = data.c_str();
  int32_t size = data.size();

  while (write_offset < offset + size) {
    callback.WaitForResult(file_io->Write(write_offset,
                                          &buf[write_offset - offset],
                                          size - write_offset + offset,
                                          callback.GetCallback()));
    if (callback.result() < 0)
      return callback.result();
    if (callback.result() == 0)
      return PP_ERROR_FAILED;
    write_offset += callback.result();
  }

  return PP_OK;
}

}  // namespace

TestURLLoader::TestURLLoader(TestingInstance* instance)
    : TestCase(instance),
      file_io_private_interface_(NULL),
      url_loader_trusted_interface_(NULL) {
}

bool TestURLLoader::Init() {
  if (!CheckTestingInterface()) {
    instance_->AppendError("Testing interface not available");
    return false;
  }

  const PPB_FileIO* file_io_interface = static_cast<const PPB_FileIO*>(
      pp::Module::Get()->GetBrowserInterface(PPB_FILEIO_INTERFACE));
  if (!file_io_interface)
    instance_->AppendError("FileIO interface not available");

  file_io_private_interface_ = static_cast<const PPB_FileIO_Private*>(
      pp::Module::Get()->GetBrowserInterface(PPB_FILEIO_PRIVATE_INTERFACE));
  if (!file_io_private_interface_)
    instance_->AppendError("FileIO_Private interface not available");
  url_loader_trusted_interface_ = static_cast<const PPB_URLLoaderTrusted*>(
      pp::Module::Get()->GetBrowserInterface(PPB_URLLOADERTRUSTED_INTERFACE));
  if (!testing_interface_->IsOutOfProcess()) {
    // Trusted interfaces are not supported under NaCl.
#if !(defined __native_client__)
    if (!url_loader_trusted_interface_)
      instance_->AppendError("URLLoaderTrusted interface not available");
#else
    if (url_loader_trusted_interface_)
      instance_->AppendError("URLLoaderTrusted interface is supported by NaCl");
#endif
  }
  return EnsureRunningOverHTTP();
}

/*
 * The test order is important here, as running tests out of order may cause
 * test timeout.
 *
 * Here is the environment:
 *
 * 1. TestServer.py only accepts one open connection at the time.
 * 2. HTTP socket pool keeps sockets open for several seconds after last use
 * (hoping that there will be another request that could reuse the connection).
 * 3. HTTP socket pool is separated by host/port and privacy mode (which is
 * based on cookies set/get permissions). So, connections to 127.0.0.1,
 * localhost and localhost in privacy mode cannot reuse existing socket and will
 * try to open another connection.
 *
 * Here is the problem:
 *
 * Original test order was repeatedly accessing 127.0.0.1, localhost and
 * localhost in privacy mode, causing new sockets to open and try to connect to
 * testserver, which they couldn't until previous connection is closed by socket
 * pool idle socket timeout (10 seconds).
 *
 * Because of this the test run was taking around 45 seconds, and test was
 * reported as 'timed out' by trybot.
 *
 * Re-ordering of tests provides more sequential access to 127.0.0.1, localhost
 * and localhost in privacy mode. It decreases the number of times when socket
 * pool doesn't have existing connection to host and has to wait, therefore
 * reducing total test time and ensuring its completion under 30 seconds.
 */
void TestURLLoader::RunTests(const std::string& filter) {
  // These tests connect to 127.0.0.1:
  RUN_CALLBACK_TEST(TestURLLoader, BasicGET, filter);
  RUN_CALLBACK_TEST(TestURLLoader, BasicPOST, filter);
  RUN_CALLBACK_TEST(TestURLLoader, BasicFilePOST, filter);
  RUN_CALLBACK_TEST(TestURLLoader, BasicFileRangePOST, filter);
  RUN_CALLBACK_TEST(TestURLLoader, CompoundBodyPOST, filter);
  RUN_CALLBACK_TEST(TestURLLoader, EmptyDataPOST, filter);
  RUN_CALLBACK_TEST(TestURLLoader, BinaryDataPOST, filter);
  RUN_CALLBACK_TEST(TestURLLoader, CustomRequestHeader, filter);
  RUN_CALLBACK_TEST(TestURLLoader, FailsBogusContentLength, filter);
  RUN_CALLBACK_TEST(TestURLLoader, StreamToFile, filter);
  RUN_CALLBACK_TEST(TestURLLoader, UntrustedJavascriptURLRestriction, filter);
  RUN_CALLBACK_TEST(TestURLLoader, TrustedJavascriptURLRestriction, filter);
  RUN_CALLBACK_TEST(TestURLLoader, UntrustedHttpRequests, filter);
  RUN_CALLBACK_TEST(TestURLLoader, TrustedHttpRequests, filter);
  RUN_CALLBACK_TEST(TestURLLoader, FollowURLRedirect, filter);
  RUN_CALLBACK_TEST(TestURLLoader, AuditURLRedirect, filter);
  RUN_CALLBACK_TEST(TestURLLoader, AbortCalls, filter);
  RUN_CALLBACK_TEST(TestURLLoader, UntendedLoad, filter);
  RUN_CALLBACK_TEST(TestURLLoader, PrefetchBufferThreshold, filter);
  // These tests connect to localhost with privacy mode enabled:
  RUN_CALLBACK_TEST(TestURLLoader, UntrustedSameOriginRestriction, filter);
  RUN_CALLBACK_TEST(TestURLLoader, UntrustedCrossOriginRequest, filter);
  // These tests connect to localhost with privacy mode disabled:
  RUN_CALLBACK_TEST(TestURLLoader, TrustedSameOriginRestriction, filter);
  RUN_CALLBACK_TEST(TestURLLoader, TrustedCrossOriginRequest, filter);
}

std::string TestURLLoader::ReadEntireFile(pp::FileIO* file_io,
                                          std::string* data) {
  TestCompletionCallback callback(instance_->pp_instance(), callback_type());
  char buf[256];
  int64_t offset = 0;

  for (;;) {
    callback.WaitForResult(file_io->Read(offset, buf, sizeof(buf),
                           callback.GetCallback()));
    if (callback.result() < 0)
      return ReportError("FileIO::Read", callback.result());
    if (callback.result() == 0)
      break;
    offset += callback.result();
    data->append(buf, callback.result());
  }

  PASS();
}

std::string TestURLLoader::ReadEntireResponseBody(pp::URLLoader* loader,
                                                  std::string* body) {
  TestCompletionCallback callback(instance_->pp_instance(), callback_type());
  char buf[2];  // Small so that multiple reads are needed.

  for (;;) {
    callback.WaitForResult(
        loader->ReadResponseBody(buf, sizeof(buf), callback.GetCallback()));
    if (callback.result() < 0)
      return ReportError("URLLoader::ReadResponseBody", callback.result());
    if (callback.result() == 0)
      break;
    body->append(buf, callback.result());
  }

  PASS();
}

std::string TestURLLoader::LoadAndCompareBody(
    const pp::URLRequestInfo& request,
    const std::string& expected_body) {
  TestCompletionCallback callback(instance_->pp_instance(), callback_type());

  pp::URLLoader loader(instance_);
  callback.WaitForResult(loader.Open(request, callback.GetCallback()));
  CHECK_CALLBACK_BEHAVIOR(callback);
  ASSERT_EQ(PP_OK, callback.result());

  pp::URLResponseInfo response_info(loader.GetResponseInfo());
  if (response_info.is_null())
    return "URLLoader::GetResponseInfo returned null";
  int32_t status_code = response_info.GetStatusCode();
  if (status_code != 200)
    return "Unexpected HTTP status code";

  std::string body;
  std::string error = ReadEntireResponseBody(&loader, &body);
  if (!error.empty())
    return error;

  if (body.size() != expected_body.size())
    return "URLLoader::ReadResponseBody returned unexpected content length";
  if (body != expected_body)
    return "URLLoader::ReadResponseBody returned unexpected content";

  PASS();
}

int32_t TestURLLoader::OpenFileSystem(pp::FileSystem* file_system,
                                      std::string* message) {
  TestCompletionCallback callback(instance_->pp_instance(), callback_type());
  callback.WaitForResult(file_system->Open(1024, callback.GetCallback()));
  if (callback.failed()) {
    message->assign(callback.errors());
    return callback.result();
  }
  if (callback.result() != PP_OK) {
    message->assign("FileSystem::Open");
    return callback.result();
  }
  return callback.result();
}

int32_t TestURLLoader::PrepareFileForPost(
      const pp::FileRef& file_ref,
      const std::string& data,
      std::string* message) {
  TestCompletionCallback callback(instance_->pp_instance(), callback_type());
  pp::FileIO file_io(instance_);
  callback.WaitForResult(file_io.Open(file_ref,
                                      PP_FILEOPENFLAG_CREATE |
                                      PP_FILEOPENFLAG_TRUNCATE |
                                      PP_FILEOPENFLAG_WRITE,
                                      callback.GetCallback()));
  if (callback.failed()) {
    message->assign(callback.errors());
    return callback.result();
  }
  if (callback.result() != PP_OK) {
    message->assign("FileIO::Open failed.");
    return callback.result();
  }

  int32_t rv = WriteEntireBuffer(instance_->pp_instance(), &file_io, 0, data,
                                 callback_type());
  if (rv != PP_OK) {
    message->assign("FileIO::Write failed.");
    return rv;
  }

  return rv;
}

std::string TestURLLoader::GetReachableAbsoluteURL(
    const std::string& file_name) {
  // Get the absolute page URL and replace the test case file name
  // with the given one.
  pp::Var document_url(
      pp::PASS_REF,
      testing_interface_->GetDocumentURL(instance_->pp_instance(),
                                         NULL));
  std::string url(document_url.AsString());
  std::string old_name("test_case.html");
  size_t index = url.find(old_name);
  ASSERT_NE(index, std::string::npos);
  url.replace(index, old_name.length(), file_name);
  return url;
}

std::string TestURLLoader::GetReachableCrossOriginURL(
    const std::string& file_name) {
  // Get an absolute URL and use it to construct a URL that will be
  // considered cross-origin by the CORS access control code, and yet be
  // reachable by the test server.
  std::string url = GetReachableAbsoluteURL(file_name);
  // Replace '127.0.0.1' with 'localhost'.
  std::string host("127.0.0.1");
  size_t index = url.find(host);
  ASSERT_NE(index, std::string::npos);
  url.replace(index, host.length(), "localhost");
  return url;
}

int32_t TestURLLoader::OpenUntrusted(const std::string& method,
                                     const std::string& header) {
  pp::URLRequestInfo request(instance_);
  request.SetURL("/echo");
  request.SetMethod(method);
  request.SetHeaders(header);

  return OpenUntrusted(request);
}

int32_t TestURLLoader::OpenTrusted(const std::string& method,
                                   const std::string& header) {
  pp::URLRequestInfo request(instance_);
  request.SetURL("/echo");
  request.SetMethod(method);
  request.SetHeaders(header);

  return OpenTrusted(request);
}

int32_t TestURLLoader::OpenUntrusted(const pp::URLRequestInfo& request) {
  return Open(request, false);
}

int32_t TestURLLoader::OpenTrusted(const pp::URLRequestInfo& request) {
  return Open(request, true);
}

int32_t TestURLLoader::Open(const pp::URLRequestInfo& request,
                            bool trusted) {
  pp::URLLoader loader(instance_);
  if (trusted)
    url_loader_trusted_interface_->GrantUniversalAccess(loader.pp_resource());
  TestCompletionCallback callback(instance_->pp_instance(), callback_type());
  callback.WaitForResult(loader.Open(request, callback.GetCallback()));
  return callback.result();
}

std::string TestURLLoader::TestBasicGET() {
  pp::URLRequestInfo request(instance_);
  request.SetURL("test_url_loader_data/hello.txt");
  return LoadAndCompareBody(request, "hello\n");
}

std::string TestURLLoader::TestBasicPOST() {
  pp::URLRequestInfo request(instance_);
  request.SetURL("/echo");
  request.SetMethod("POST");
  std::string postdata("postdata");
  request.AppendDataToBody(postdata.data(), postdata.length());
  return LoadAndCompareBody(request, postdata);
}

std::string TestURLLoader::TestBasicFilePOST() {
  std::string message;

  pp::FileSystem file_system(instance_, PP_FILESYSTEMTYPE_LOCALTEMPORARY);
  int32_t rv = OpenFileSystem(&file_system, &message);
  if (rv != PP_OK)
    return ReportError(message.c_str(), rv);

  pp::FileRef file_ref(file_system, "/file_post_test");
  std::string postdata("postdata");
  rv = PrepareFileForPost(file_ref, postdata, &message);
  if (rv != PP_OK)
    return ReportError(message.c_str(), rv);

  pp::URLRequestInfo request(instance_);
  request.SetURL("/echo");
  request.SetMethod("POST");
  request.AppendFileToBody(file_ref, 0);
  return LoadAndCompareBody(request, postdata);
}

std::string TestURLLoader::TestBasicFileRangePOST() {
  std::string message;

  pp::FileSystem file_system(instance_, PP_FILESYSTEMTYPE_LOCALTEMPORARY);
  int32_t rv = OpenFileSystem(&file_system, &message);
  if (rv != PP_OK)
    return ReportError(message.c_str(), rv);

  pp::FileRef file_ref(file_system, "/file_range_post_test");
  std::string postdata("postdatapostdata");
  rv = PrepareFileForPost(file_ref, postdata, &message);
  if (rv != PP_OK)
    return ReportError(message.c_str(), rv);

  pp::URLRequestInfo request(instance_);
  request.SetURL("/echo");
  request.SetMethod("POST");
  request.AppendFileRangeToBody(file_ref, 4, 12, 0);
  return LoadAndCompareBody(request, postdata.substr(4, 12));
}

std::string TestURLLoader::TestCompoundBodyPOST() {
  pp::URLRequestInfo request(instance_);
  request.SetURL("/echo");
  request.SetMethod("POST");
  std::string postdata1("post");
  request.AppendDataToBody(postdata1.data(), postdata1.length());
  std::string postdata2("data");
  request.AppendDataToBody(postdata2.data(), postdata2.length());
  return LoadAndCompareBody(request, postdata1 + postdata2);
}

std::string TestURLLoader::TestEmptyDataPOST() {
  pp::URLRequestInfo request(instance_);
  request.SetURL("/echo");
  request.SetMethod("POST");
  request.AppendDataToBody("", 0);
  return LoadAndCompareBody(request, std::string());
}

std::string TestURLLoader::TestBinaryDataPOST() {
  pp::URLRequestInfo request(instance_);
  request.SetURL("/echo");
  request.SetMethod("POST");
  const char postdata_chars[] =
      "\x00\x01\x02\x03\x04\x05postdata\xfa\xfb\xfc\xfd\xfe\xff";
  std::string postdata(postdata_chars,
                       sizeof(postdata_chars) / sizeof(postdata_chars[0]));
  request.AppendDataToBody(postdata.data(), postdata.length());
  return LoadAndCompareBody(request, postdata);
}

std::string TestURLLoader::TestCustomRequestHeader() {
  pp::URLRequestInfo request(instance_);
  request.SetURL("/echoheader?Foo");
  request.SetHeaders("Foo: 1");
  return LoadAndCompareBody(request, "1");
}

std::string TestURLLoader::TestFailsBogusContentLength() {
  pp::URLRequestInfo request(instance_);
  request.SetURL("/echo");
  request.SetMethod("POST");
  request.SetHeaders("Content-Length: 400");
  std::string postdata("postdata");
  request.AppendDataToBody(postdata.data(), postdata.length());

  int32_t rv;
  rv = OpenUntrusted(request);
  if (rv != PP_ERROR_NOACCESS)
    return ReportError(
        "Untrusted request with bogus Content-Length restriction", rv);

  PASS();
}

std::string TestURLLoader::TestStreamToFile() {
  pp::URLRequestInfo request(instance_);
  request.SetURL("test_url_loader_data/hello.txt");
  request.SetStreamToFile(true);

  TestCompletionCallback callback(instance_->pp_instance(), callback_type());

  pp::URLLoader loader(instance_);
  callback.WaitForResult(loader.Open(request, callback.GetCallback()));
  CHECK_CALLBACK_BEHAVIOR(callback);
  ASSERT_EQ(PP_OK, callback.result());

  pp::URLResponseInfo response_info(loader.GetResponseInfo());
  if (response_info.is_null())
    return "URLLoader::GetResponseInfo returned null";
  int32_t status_code = response_info.GetStatusCode();
  if (status_code != 200)
    return "Unexpected HTTP status code";

  pp::FileRef body(response_info.GetBodyAsFileRef());
  if (body.is_null())
    return "URLResponseInfo::GetBody returned null";

  callback.WaitForResult(loader.FinishStreamingToFile(callback.GetCallback()));
  CHECK_CALLBACK_BEHAVIOR(callback);
  ASSERT_EQ(PP_OK, callback.result());

  pp::FileIO reader(instance_);
  callback.WaitForResult(reader.Open(body, PP_FILEOPENFLAG_READ,
                                     callback.GetCallback()));
  CHECK_CALLBACK_BEHAVIOR(callback);
  ASSERT_EQ(PP_OK, callback.result());

  std::string data;
  std::string error = ReadEntireFile(&reader, &data);
  if (!error.empty())
    return error;

  std::string expected_body = "hello\n";
  if (data.size() != expected_body.size())
    return "ReadEntireFile returned unexpected content length";
  if (data != expected_body)
    return "ReadEntireFile returned unexpected content";

  PASS();
}

// Untrusted, unintended cross-origin requests should fail.
std::string TestURLLoader::TestUntrustedSameOriginRestriction() {
  pp::URLRequestInfo request(instance_);
  std::string cross_origin_url = GetReachableCrossOriginURL("test_case.html");
  request.SetURL(cross_origin_url);

  int32_t rv = OpenUntrusted(request);
  if (rv != PP_ERROR_NOACCESS)
    return ReportError(
        "Untrusted, unintended cross-origin request restriction", rv);

  PASS();
}

// Trusted, unintended cross-origin requests should succeed.
std::string TestURLLoader::TestTrustedSameOriginRestriction() {
  pp::URLRequestInfo request(instance_);
  std::string cross_origin_url = GetReachableCrossOriginURL("test_case.html");
  request.SetURL(cross_origin_url);

  int32_t rv = OpenTrusted(request);
  if (rv != PP_OK)
    return ReportError("Trusted cross-origin request failed", rv);

  PASS();
}

// Untrusted, intended cross-origin requests should use CORS and succeed.
std::string TestURLLoader::TestUntrustedCrossOriginRequest() {
  pp::URLRequestInfo request(instance_);
  std::string cross_origin_url = GetReachableCrossOriginURL("test_case.html");
  request.SetURL(cross_origin_url);
  request.SetAllowCrossOriginRequests(true);

  int32_t rv = OpenUntrusted(request);
  if (rv != PP_OK)
    return ReportError(
        "Untrusted, intended cross-origin request failed", rv);

  PASS();
}

// Trusted, intended cross-origin requests should use CORS and succeed.
std::string TestURLLoader::TestTrustedCrossOriginRequest() {
  pp::URLRequestInfo request(instance_);
  std::string cross_origin_url = GetReachableCrossOriginURL("test_case.html");
  request.SetURL(cross_origin_url);
  request.SetAllowCrossOriginRequests(true);

  int32_t rv = OpenTrusted(request);
  if (rv != PP_OK)
    return ReportError("Trusted cross-origin request failed", rv);

  PASS();
}

// Untrusted Javascript URLs requests should fail.
std::string TestURLLoader::TestUntrustedJavascriptURLRestriction() {
  pp::URLRequestInfo request(instance_);
  request.SetURL("javascript:foo = bar");

  int32_t rv = OpenUntrusted(request);
  if (rv != PP_ERROR_NOACCESS)
    return ReportError(
        "Untrusted Javascript URL request restriction failed", rv);

  PASS();
}

// Trusted Javascript URLs requests should succeed.
std::string TestURLLoader::TestTrustedJavascriptURLRestriction() {
  pp::URLRequestInfo request(instance_);
  request.SetURL("javascript:foo = bar");

  int32_t rv = OpenTrusted(request);
  if (rv == PP_ERROR_NOACCESS)
  return ReportError(
      "Trusted Javascript URL request", rv);

  PASS();
}

std::string TestURLLoader::TestUntrustedHttpRequests() {
  // HTTP methods are restricted only for untrusted loaders. Forbidden
  // methods are CONNECT, TRACE, and TRACK, and any string that is not a
  // valid token (containing special characters like CR, LF).
  // http://www.w3.org/TR/XMLHttpRequest/
  {
    ASSERT_EQ(OpenUntrusted("cOnNeCt", std::string()), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("tRaCk", std::string()), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("tRaCe", std::string()), PP_ERROR_NOACCESS);
    ASSERT_EQ(
        OpenUntrusted("POST\x0d\x0ax-csrf-token:\x20test1234", std::string()),
        PP_ERROR_NOACCESS);
  }
  // HTTP methods are restricted only for untrusted loaders. Try all headers
  // that are forbidden by http://www.w3.org/TR/XMLHttpRequest/.
  {
    ASSERT_EQ(OpenUntrusted("GET", "Accept-Charset:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "Accept-Encoding:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "Connection:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "Content-Length:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "Cookie:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "Cookie2:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted(
        "GET", "Content-Transfer-Encoding:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "Date:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "Expect:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "Host:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "Keep-Alive:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "Referer:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "TE:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "Trailer:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted(
        "GET", "Transfer-Encoding:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "Upgrade:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "User-Agent:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "Via:\n"), PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted(
        "GET", "Proxy-Authorization: Basic dXNlcjpwYXNzd29yZA==:\n"),
            PP_ERROR_NOACCESS);
    ASSERT_EQ(OpenUntrusted("GET", "Sec-foo:\n"), PP_ERROR_NOACCESS);
  }
  // Untrusted requests with custom referrer should fail.
  {
    pp::URLRequestInfo request(instance_);
    request.SetCustomReferrerURL("http://www.google.com/");

    int32_t rv = OpenUntrusted(request);
    if (rv != PP_ERROR_NOACCESS)
      return ReportError(
          "Untrusted request with custom referrer restriction", rv);
  }
  // Untrusted requests with custom transfer encodings should fail.
  {
    pp::URLRequestInfo request(instance_);
    request.SetCustomContentTransferEncoding("foo");

    int32_t rv = OpenUntrusted(request);
    if (rv != PP_ERROR_NOACCESS)
      return ReportError(
          "Untrusted request with content-transfer-encoding restriction", rv);
  }

  PASS();
}

std::string TestURLLoader::TestTrustedHttpRequests() {
  // Trusted requests can use restricted methods.
  {
    ASSERT_EQ(OpenTrusted("cOnNeCt", std::string()), PP_OK);
    ASSERT_EQ(OpenTrusted("tRaCk", std::string()), PP_OK);
    ASSERT_EQ(OpenTrusted("tRaCe", std::string()), PP_OK);
  }
  // Trusted requests can use restricted headers.
  {
    ASSERT_EQ(OpenTrusted("GET", "Accept-Charset:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "Accept-Encoding:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "Connection:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "Content-Length:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "Cookie:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "Cookie2:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted(
        "GET", "Content-Transfer-Encoding:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "Date:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "Expect:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "Host:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "Keep-Alive:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "Referer:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "TE:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "Trailer:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "Transfer-Encoding:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "Upgrade:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "User-Agent:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "Via:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted(
        "GET", "Proxy-Authorization: Basic dXNlcjpwYXNzd29yZA==:\n"), PP_OK);
    ASSERT_EQ(OpenTrusted("GET", "Sec-foo:\n"), PP_OK);
  }
  // Trusted requests with custom referrer should succeed.
  {
    pp::URLRequestInfo request(instance_);
    request.SetCustomReferrerURL("http://www.google.com/");

    int32_t rv = OpenTrusted(request);
    if (rv != PP_OK)
      return ReportError("Trusted request with custom referrer", rv);
  }
  // Trusted requests with custom transfer encodings should succeed.
  {
    pp::URLRequestInfo request(instance_);
    request.SetCustomContentTransferEncoding("foo");

    int32_t rv = OpenTrusted(request);
    if (rv != PP_OK)
      return ReportError(
          "Trusted request with content-transfer-encoding failed", rv);
  }

  PASS();
}

// This test should cause a redirect and ensure that the loader follows it.
std::string TestURLLoader::TestFollowURLRedirect() {
  pp::URLRequestInfo request(instance_);
  // This prefix causes the test server to return a 301 redirect.
  std::string redirect_prefix("/server-redirect?");
  // We need an absolute path for the redirect to actually work.
  std::string redirect_url =
      GetReachableAbsoluteURL("test_url_loader_data/hello.txt");
  request.SetURL(redirect_prefix.append(redirect_url));
  return LoadAndCompareBody(request, "hello\n");
}

// This test should cause a redirect and ensure that the loader runs
// the callback, rather than following the redirect.
std::string TestURLLoader::TestAuditURLRedirect() {
  pp::URLRequestInfo request(instance_);
  // This path will cause the server to return a 301 redirect.
  // This prefix causes the test server to return a 301 redirect.
  std::string redirect_prefix("/server-redirect?");
  // We need an absolute path for the redirect to actually work.
  std::string redirect_url =
      GetReachableAbsoluteURL("test_url_loader_data/hello.txt");
  request.SetURL(redirect_prefix.append(redirect_url));
  request.SetFollowRedirects(false);

  TestCompletionCallback callback(instance_->pp_instance(), callback_type());

  pp::URLLoader loader(instance_);
  callback.WaitForResult(loader.Open(request, callback.GetCallback()));
  CHECK_CALLBACK_BEHAVIOR(callback);
  ASSERT_EQ(PP_OK, callback.result());

  // Checks that the response indicates a redirect, and that the URL
  // is correct.
  pp::URLResponseInfo response_info(loader.GetResponseInfo());
  if (response_info.is_null())
    return "URLLoader::GetResponseInfo returned null";
  int32_t status_code = response_info.GetStatusCode();
  if (status_code != 301)
    return "Response status should be 301";

  // Test that the paused loader can be resumed.
  callback.WaitForResult(loader.FollowRedirect(callback.GetCallback()));
  CHECK_CALLBACK_BEHAVIOR(callback);
  ASSERT_EQ(PP_OK, callback.result());
  std::string body;
  std::string error = ReadEntireResponseBody(&loader, &body);
  if (!error.empty())
    return error;

  if (body != "hello\n")
    return "URLLoader::FollowRedirect failed";

  PASS();
}

std::string TestURLLoader::TestAbortCalls() {
  pp::URLRequestInfo request(instance_);
  request.SetURL("test_url_loader_data/hello.txt");

  TestCompletionCallback callback(instance_->pp_instance(), callback_type());
  int32_t rv;

  // Abort |Open()|.
  {
    rv = pp::URLLoader(instance_).Open(request, callback.GetCallback());
  }
  callback.WaitForAbortResult(rv);
  CHECK_CALLBACK_BEHAVIOR(callback);

  // Abort |ReadResponseBody()|.
  {
    char buf[2] = { 0 };
    {
      pp::URLLoader loader(instance_);
      callback.WaitForResult(loader.Open(request, callback.GetCallback()));
      CHECK_CALLBACK_BEHAVIOR(callback);
      ASSERT_EQ(PP_OK, callback.result());

      rv = loader.ReadResponseBody(buf, sizeof(buf), callback.GetCallback());
    }  // Destroy |loader|.
    callback.WaitForAbortResult(rv);
    CHECK_CALLBACK_BEHAVIOR(callback);
    if (rv == PP_OK_COMPLETIONPENDING) {
      if (buf[0] || buf[1]) {
        return "URLLoader::ReadResponseBody wrote data after resource "
               "destruction.";
      }
    }
  }

  // TODO(viettrungluu): More abort tests (but add basic tests first).
  // Also test that Close() aborts properly. crbug.com/69457

  PASS();
}

std::string TestURLLoader::TestUntendedLoad() {
  pp::URLRequestInfo request(instance_);
  request.SetURL("test_url_loader_data/hello.txt");
  request.SetRecordDownloadProgress(true);
  TestCompletionCallback callback(instance_->pp_instance(), callback_type());

  pp::URLLoader loader(instance_);
  callback.WaitForResult(loader.Open(request, callback.GetCallback()));
  CHECK_CALLBACK_BEHAVIOR(callback);
  ASSERT_EQ(PP_OK, callback.result());

  // We received the response callback. Yield until the network code has called
  // the loader's didReceiveData and didFinishLoading methods before we give it
  // another callback function, to make sure the loader works with no callback.
  int64_t bytes_received = 0;
  int64_t total_bytes_to_be_received = 0;
  while (true) {
    loader.GetDownloadProgress(&bytes_received, &total_bytes_to_be_received);
    if (total_bytes_to_be_received <= 0)
      return ReportError("URLLoader::GetDownloadProgress total size",
          total_bytes_to_be_received);
    if (bytes_received == total_bytes_to_be_received)
      break;
    // Yield if we're on the main thread, so that URLLoader can receive more
    // data.
    if (pp::Module::Get()->core()->IsMainThread()) {
      NestedEvent event(instance_->pp_instance());
      event.PostSignal(10);
      event.Wait();
    }
  }
  // The loader should now have the data and have finished successfully.
  std::string body;
  std::string error = ReadEntireResponseBody(&loader, &body);
  if (!error.empty())
    return error;
  if (body != "hello\n")
    return ReportError("Couldn't read data", callback.result());

  PASS();
}

int32_t TestURLLoader::OpenWithPrefetchBufferThreshold(int32_t lower,
                                                       int32_t upper) {
  pp::URLRequestInfo request(instance_);
  request.SetURL("test_url_loader_data/hello.txt");
  request.SetPrefetchBufferLowerThreshold(lower);
  request.SetPrefetchBufferUpperThreshold(upper);

  return OpenUntrusted(request);
}

std::string TestURLLoader::TestPrefetchBufferThreshold() {
  int32_t rv = OpenWithPrefetchBufferThreshold(-1, 1);
  if (rv != PP_ERROR_FAILED) {
    return ReportError("The prefetch limits contained a negative value but "
                       "the URLLoader did not fail.", rv);
  }

  rv = OpenWithPrefetchBufferThreshold(0, 1);
  if (rv != PP_OK) {
    return ReportError("The prefetch buffer limits were legal values but "
                       "the URLLoader failed.", rv);
  }

  rv = OpenWithPrefetchBufferThreshold(1000, 1);
  if (rv != PP_ERROR_FAILED) {
    return ReportError("The lower buffer value was higher than the upper but "
                       "the URLLoader did not fail.", rv);
  }

  PASS();
}

// TODO(viettrungluu): Add tests for  Get{Upload,Download}Progress, Close
// (including abort tests if applicable).
