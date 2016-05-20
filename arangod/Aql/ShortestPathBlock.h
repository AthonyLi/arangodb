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

#ifndef ARANGOD_AQL_SHORTEST_PATH_BLOCK_H
#define ARANGOD_AQL_SHORTEST_PATH_BLOCK_H 1

#include "Aql/ExecutionBlock.h"
#include "Aql/ShortestPathNode.h"

namespace arangodb {
namespace aql {

class ShortestPathNode;

class ShortestPathBlock : public ExecutionBlock {

 public:
  ShortestPathBlock(ExecutionEngine* engine, ShortestPathNode const* ep);

  ~ShortestPathBlock();

  /// @brief initialize, here we fetch all docs from the database
  int initialize() override;

  /// @brief initializeCursor
  int initializeCursor(AqlItemBlock* items, size_t pos) override;

  /// @brief getSome
  AqlItemBlock* getSome(size_t atLeast, size_t atMost) override final;

  // skip between atLeast and atMost, returns the number actually skipped . . .
  // will only return less than atLeast if there aren't atLeast many
  // things to skip overall.
  size_t skipSome(size_t atLeast, size_t atMost) override final;

};

} // namespace arangodb::aql
} // namespace arangodb
#endif
