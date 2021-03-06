////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016-2016 ArangoDB GmbH, Cologne, Germany
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

#ifndef ARANGOD_AQL_SHORTEST_PATH_OPTIONS_H
#define ARANGOD_AQL_SHORTEST_PATH_OPTIONS_H 1

#include "Basics/Common.h"
#include "Basics/JsonHelper.h"

namespace arangodb {
namespace aql {

/// @brief TraversalOptions
struct ShortestPathOptions {

  /// @brief constructor, using JSON
  ShortestPathOptions(arangodb::basics::Json const&);

  /// @brief constructor, using default values
  ShortestPathOptions()
      : weightAttribute(""),
        defaultWeight(1) {}

  void toJson(arangodb::basics::Json&, TRI_memory_zone_t*) const;

  void toVelocyPack(arangodb::velocypack::Builder&) const;

  std::string weightAttribute;
  double defaultWeight;
};

}  // namespace arangodb::aql
}  // namespace arangodb
#endif

