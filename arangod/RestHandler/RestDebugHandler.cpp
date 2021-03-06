////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "RestDebugHandler.h"

#include "Rest/HttpRequest.h"
#include "Rest/Version.h"

using namespace arangodb;

using namespace arangodb::basics;
using namespace arangodb::rest;

////////////////////////////////////////////////////////////////////////////////
/// @brief ArangoDB server
////////////////////////////////////////////////////////////////////////////////

RestDebugHandler::RestDebugHandler(GeneralRequest* request,
                                   GeneralResponse* response)
    : RestVocbaseBaseHandler(request, response) {}

bool RestDebugHandler::isDirect() const { return false; }

RestHandler::status RestDebugHandler::execute() {
  // extract the sub-request type
  auto const type = _request->requestType();
  size_t const len = _request->suffix().size();

  if (len == 0 || len > 2 || !(_request->suffix()[0] == "failat")) {
    generateNotImplemented("ILLEGAL /_admin/debug/failat");
    return status::DONE;
  }
  std::vector<std::string> const& suffix = _request->suffix();

  // execute one of the CRUD methods
  switch (type) {
    case GeneralRequest::RequestType::DELETE_REQ:
      if (len == 1) {
        TRI_ClearFailurePointsDebugging();
      } else {
        TRI_RemoveFailurePointDebugging(suffix[1].c_str());
      }
      break;
    case GeneralRequest::RequestType::PUT:
      if (len == 2) {
        TRI_AddFailurePointDebugging(suffix[1].c_str());
      } else {
        generateNotImplemented("ILLEGAL /_admin/debug/failat");
      }
      break;
    default:
      generateNotImplemented("ILLEGAL /_admin/debug/failat");
      return status::DONE;
  }
  try {
    VPackBuilder result;
    result.add(VPackValue(true));
    generateResult(GeneralResponse::ResponseCode::OK, result.slice());
  } catch (...) {
    // Ignore this error
  }
  return status::DONE;
}
