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
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#include "Transaction.h"
#include "Aql/Ast.h"
#include "Aql/AstNode.h"
#include "Aql/Condition.h"
#include "Aql/SortCondition.h"
#include "Basics/AttributeNameParser.h"
#include "Basics/Exceptions.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterComm.h"
#include "Cluster/ClusterMethods.h"
#include "Cluster/ServerState.h"
#include "Indexes/PrimaryIndex.h"
#include "Indexes/EdgeIndex.h"
#include "Indexes/HashIndex.h"
#include "Indexes/RocksDBIndex.h"
#include "Indexes/SkiplistIndex.h"
#include "Logger/Logger.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/OperationCursor.h"
#include "Utils/TransactionContext.h"
#include "VocBase/DatafileHelper.h"
#include "VocBase/Ditch.h"
#include "VocBase/document-collection.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/MasterPointers.h"
#include "VocBase/server.h"

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Options.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;

//////////////////////////////////////////////////////////////////////////////
/// @brief IndexHandle getter method
//////////////////////////////////////////////////////////////////////////////

std::shared_ptr<arangodb::Index> Transaction::IndexHandle::getIndex() const {
  return _index;
}

//////////////////////////////////////////////////////////////////////////////
/// @brief IndexHandle toVelocyPack method passthrough
//////////////////////////////////////////////////////////////////////////////

void Transaction::IndexHandle::toVelocyPack(
    arangodb::velocypack::Builder& builder,
    bool withFigures) const {
  _index->toVelocyPack(builder, withFigures);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief tests if the given index supports the sort condition
//////////////////////////////////////////////////////////////////////////////

static bool indexSupportsSort(Index const* idx, arangodb::aql::Variable const* reference,
                              arangodb::aql::SortCondition const* sortCondition,
                              size_t itemsInIndex,
                              double& estimatedCost,
                              size_t& coveredAttributes) {
  if (idx->isSorted() &&
      idx->supportsSortCondition(sortCondition, reference, itemsInIndex,
                                 estimatedCost, coveredAttributes)) {
    // index supports the sort condition
    return true;
  }

  // index does not support the sort condition
  if (itemsInIndex > 0) {
    estimatedCost = itemsInIndex * std::log2(static_cast<double>(itemsInIndex));
  } else {
    estimatedCost = 0.0;
  }
  return false;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Return an Operation Result that parses the error information returned
///        by the DBServer.
////////////////////////////////////////////////////////////////////////////////

static OperationResult DBServerResponseBad(std::shared_ptr<VPackBuilder> resultBody) {
  VPackSlice res = resultBody->slice();
  return OperationResult(
      arangodb::basics::VelocyPackHelper::getNumericValue<int>(
          res, "errorNum", TRI_ERROR_INTERNAL),
      arangodb::basics::VelocyPackHelper::getStringValue(
          res, "errorMessage", "JSON sent to DBserver was bad"));
}

static OperationResult DBServerResponseBad(std::string const& resultBody) {
  // TODO DEPRECATED
  // The body contains more information so we parse it.
  VPackParser parser;
  try {
    parser.parse(resultBody);
    return DBServerResponseBad(parser.steal());
  } catch (...) {
    return OperationResult(TRI_ERROR_INTERNAL, "JSON sent to DBserver was bad");
  }
}


////////////////////////////////////////////////////////////////////////////////
/// @brief Insert an errror reported instead of the new document
////////////////////////////////////////////////////////////////////////////////

static void createBabiesError(VPackBuilder& builder,
                              std::unordered_map<int, size_t>& countErrorCodes,
                              int errorCode) {
  builder.openObject();
  builder.add("error", VPackValue(true));
  builder.add("errorNum", VPackValue(errorCode));
  builder.close();

  auto it = countErrorCodes.find(errorCode);
  if (it == countErrorCodes.end()) {
    countErrorCodes.emplace(errorCode, 1);
  } else {
    it->second++;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sort ORs for the same attribute so they are in ascending value
/// order. this will only work if the condition is for a single attribute
/// the usedIndexes vector may also be re-sorted
////////////////////////////////////////////////////////////////////////////////

bool Transaction::sortOrs(arangodb::aql::Ast* ast,
                    arangodb::aql::AstNode* root,
                    arangodb::aql::Variable const* variable,
                    std::vector<arangodb::Transaction::IndexHandle>& usedIndexes) const {
  if (root == nullptr) {
    return true;
  }

  size_t const n = root->numMembers();

  if (n < 2) {
    return true;
  }

  if (n != usedIndexes.size()) {
    // sorting will break if the number of ORs is unequal to the number of
    // indexes
    // but we shouldn't have got here then
    TRI_ASSERT(false);
    return false;
  }

  typedef std::pair<arangodb::aql::AstNode*, arangodb::Transaction::IndexHandle> ConditionData;
  std::vector<ConditionData*> conditionData;

  auto cleanup = [&conditionData]() -> void {
    for (auto& it : conditionData) {
      delete it;
    }
  };

  TRI_DEFER(cleanup());

  std::vector<arangodb::aql::ConditionPart> parts;
  parts.reserve(n);

  for (size_t i = 0; i < n; ++i) {
    // sort the conditions of each AND
    auto sub = root->getMemberUnchecked(i);

    TRI_ASSERT(sub != nullptr &&
               sub->type ==
                   arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_NARY_AND);
    size_t const nAnd = sub->numMembers();

    if (nAnd != 1) {
      // we can't handle this one
      return false;
    }

    auto operand = sub->getMemberUnchecked(0);

    if (!operand->isComparisonOperator()) {
      return false;
    }

    if (operand->type == arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_BINARY_NE ||
        operand->type == arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_BINARY_NIN) {
      return false;
    }

    auto lhs = operand->getMember(0);
    auto rhs = operand->getMember(1);

    if (lhs->type == arangodb::aql::AstNodeType::NODE_TYPE_ATTRIBUTE_ACCESS) {
      std::pair<arangodb::aql::Variable const*, std::vector<arangodb::basics::AttributeName>>
          result;

      if (rhs->isConstant() && lhs->isAttributeAccessForVariable(result) &&
          result.first == variable &&
          (operand->type != arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_BINARY_IN || rhs->isArray())) {
        // create the condition data struct on the heap
        auto data = std::make_unique<ConditionData>(sub, usedIndexes[i]);
        // push it into an owning vector
        conditionData.emplace_back(data.get());
        // vector is now responsible for data
        auto p = data.release();
        // also add the pointer to the (non-owning) parts vector
        parts.emplace_back(arangodb::aql::ConditionPart(
            result.first, result.second, operand,
            arangodb::aql::AttributeSideType::ATTRIBUTE_LEFT, p));
      }
    }

    if (rhs->type == arangodb::aql::AstNodeType::NODE_TYPE_ATTRIBUTE_ACCESS ||
        rhs->type == arangodb::aql::AstNodeType::NODE_TYPE_EXPANSION) {
      std::pair<arangodb::aql::Variable const*, std::vector<arangodb::basics::AttributeName>>
          result;

      if (lhs->isConstant() && rhs->isAttributeAccessForVariable(result) &&
          result.first == variable) {
        // create the condition data struct on the heap
        auto data = std::make_unique<ConditionData>(sub, usedIndexes[i]);
        // push it into an owning vector
        conditionData.emplace_back(data.get());
        // vector is now responsible for data
        auto p = data.release();
        // also add the pointer to the (non-owning) parts vector
        parts.emplace_back(arangodb::aql::ConditionPart(
            result.first, result.second, operand,
            arangodb::aql::AttributeSideType::ATTRIBUTE_RIGHT, p));
      }
    }
  }

  if (parts.size() != root->numMembers()) {
    return false;
  }

  // check if all parts use the same variable and attribute
  for (size_t i = 1; i < n; ++i) {
    auto& lhs = parts[i - 1];
    auto& rhs = parts[i];

    if (lhs.variable != rhs.variable ||
        lhs.attributeName != rhs.attributeName) {
      // oops, the different OR parts are on different variables or attributes
      return false;
    }
  }

  size_t previousIn = SIZE_MAX;

  for (size_t i = 0; i < n; ++i) {
    auto& p = parts[i];

    if (p.operatorType ==
            arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_BINARY_IN &&
        p.valueNode->isArray()) {
      TRI_ASSERT(p.valueNode->isConstant());

      if (previousIn != SIZE_MAX) {
        // merge IN with IN
        TRI_ASSERT(previousIn < i);
        auto emptyArray = ast->createNodeArray();
        auto mergedIn = ast->createNodeUnionizedArray(
            parts[previousIn].valueNode, p.valueNode);
        parts[previousIn].valueNode = mergedIn;
        parts[i].valueNode = emptyArray;
        root->getMember(previousIn)->getMember(0)->changeMember(1, mergedIn);
        root->getMember(i)->getMember(0)->changeMember(1, emptyArray);
      } else {
        // note first IN
        previousIn = i;
      }
    }
  }

  // now sort all conditions by variable name, attribute name, attribute value
  std::sort(parts.begin(), parts.end(),
            [](arangodb::aql::ConditionPart const& lhs,
               arangodb::aql::ConditionPart const& rhs) -> bool {
              // compare variable names first
              auto res = lhs.variable->name.compare(rhs.variable->name);

              if (res != 0) {
                return res < 0;
              }

              // compare attribute names next
              res = lhs.attributeName.compare(rhs.attributeName);

              if (res != 0) {
                return res < 0;
              }

              // compare attribute values next
              auto ll = lhs.lowerBound();
              auto lr = rhs.lowerBound();

              if (ll == nullptr && lr != nullptr) {
                // left lower bound is not set but right
                return true;
              } else if (ll != nullptr && lr == nullptr) {
                // left lower bound is set but not right
                return false;
              }

              if (ll != nullptr && lr != nullptr) {
                // both lower bounds are set
                res = CompareAstNodes(ll, lr, true);

                if (res != 0) {
                  return res < 0;
                }
              }

              if (lhs.isLowerInclusive() && !rhs.isLowerInclusive()) {
                return true;
              }
              if (rhs.isLowerInclusive() && !lhs.isLowerInclusive()) {
                return false;
              }

              // all things equal
              return false;
            });

  TRI_ASSERT(parts.size() == conditionData.size());

  // clean up
  usedIndexes.clear();
  while (root->numMembers()) {
    root->removeMemberUnchecked(0);
  }

  // and rebuild
  for (size_t i = 0; i < n; ++i) {
    if (parts[i].operatorType == arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_BINARY_IN &&
        parts[i].valueNode->isArray() &&
        parts[i].valueNode->numMembers() == 0) {
      // can optimize away empty IN array
      continue;
    }

    auto conditionData = static_cast<ConditionData*>(parts[i].data);
    root->addMember(conditionData->first);
    usedIndexes.emplace_back(conditionData->second);
  }

  return true;
}

std::pair<bool, bool> Transaction::findIndexHandleForAndNode(
    std::vector<std::shared_ptr<Index>> indexes, arangodb::aql::AstNode* node,
    arangodb::aql::Variable const* reference,
    arangodb::aql::SortCondition const* sortCondition,
    size_t itemsInCollection,
    std::vector<Transaction::IndexHandle>& usedIndexes,
    arangodb::aql::AstNode*& specializedCondition,
    bool& isSparse) const {
  std::shared_ptr<Index> bestIndex;
  double bestCost = 0.0;
  bool bestSupportsFilter = false;
  bool bestSupportsSort = false;
  size_t coveredAttributes = 0;

  for (auto const& idx : indexes) {
    double filterCost = 0.0;
    double sortCost = 0.0;
    size_t itemsInIndex = itemsInCollection;

    bool supportsFilter = false;
    bool supportsSort = false;

    // check if the index supports the filter expression
    double estimatedCost;
    size_t estimatedItems;
    if (idx->supportsFilterCondition(node, reference, itemsInIndex,
                                     estimatedItems, estimatedCost)) {
      // index supports the filter condition
      filterCost = estimatedCost;
      // this reduces the number of items left
      itemsInIndex = estimatedItems;
      supportsFilter = true;
    } else {
      // index does not support the filter condition
      filterCost = itemsInIndex * 1.5;
    }

    bool const isOnlyAttributeAccess =
        (!sortCondition->isEmpty() && sortCondition->isOnlyAttributeAccess());

    if (sortCondition->isUnidirectional()) {
      // only go in here if we actually have a sort condition and it can in
      // general be supported by an index. for this, a sort condition must not
      // be empty, must consist only of attribute access, and all attributes
      // must be sorted in the direction
      if (indexSupportsSort(idx.get(), reference, sortCondition, itemsInIndex,
                            sortCost, coveredAttributes)) {
        supportsSort = true;
      }
    }

    if (!supportsSort && isOnlyAttributeAccess && node->isOnlyEqualityMatch()) {
      // index cannot be used for sorting, but the filter condition consists
      // only of equality lookups (==)
      // now check if the index fields are the same as the sort condition fields
      // e.g. FILTER c.value1 == 1 && c.value2 == 42 SORT c.value1, c.value2
      size_t coveredFields =
          sortCondition->coveredAttributes(reference, idx->fields());

      if (coveredFields == sortCondition->numAttributes() &&
          (idx->isSorted() ||
           idx->fields().size() == sortCondition->numAttributes())) {
        // no sorting needed
        sortCost = 0.0;
      }
    }

    if (!supportsFilter && !supportsSort) {
      continue;
    }

    double const totalCost = filterCost + sortCost;
    if (bestIndex == nullptr || totalCost < bestCost) {
      bestIndex = idx;
      bestCost = totalCost;
      bestSupportsFilter = supportsFilter;
      bestSupportsSort = supportsSort;
    }
  }

  if (bestIndex == nullptr) {
    return std::make_pair(false, false);
  }

  specializedCondition = bestIndex->specializeCondition(node, reference);

  usedIndexes.emplace_back(bestIndex);
  isSparse = bestIndex->sparse();

  return std::make_pair(bestSupportsFilter, bestSupportsSort);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief if this pointer is set to an actual set, then for each request
/// sent to a shardId using the ClusterComm library, an X-Arango-Nolock
/// header is generated.
////////////////////////////////////////////////////////////////////////////////

thread_local std::unordered_set<std::string>* Transaction::_makeNolockHeaders =
    nullptr;
  
////////////////////////////////////////////////////////////////////////////////
/// @brief Index Iterator Context
////////////////////////////////////////////////////////////////////////////////

struct OpenIndexIteratorContext {
  arangodb::Transaction* trx;
  TRI_document_collection_t* collection;
};
      
Transaction::Transaction(std::shared_ptr<TransactionContext> transactionContext,
                         TRI_voc_tid_t externalId)
    : _externalId(externalId),
      _setupState(TRI_ERROR_NO_ERROR),
      _nestingLevel(0),
      _errorData(),
      _hints(0),
      _timeout(0.0),
      _waitForSync(false),
      _allowImplicitCollections(true),
      _isReal(true),
      _trx(nullptr),
      _vocbase(transactionContext->vocbase()),
      _transactionContext(transactionContext) {
  TRI_ASSERT(_vocbase != nullptr);
  TRI_ASSERT(_transactionContext != nullptr);

  if (ServerState::instance()->isCoordinator()) {
    _isReal = false;
  }

  this->setupTransaction();
}
   
////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the transaction
////////////////////////////////////////////////////////////////////////////////

Transaction::~Transaction() {
  if (_trx == nullptr) {
    return;
  }

  if (isEmbeddedTransaction()) {
    _trx->_nestingLevel--;
  } else {
    if (getStatus() == TRI_TRANSACTION_RUNNING) {
      // auto abort a running transaction
      this->abort();
    }

    // free the data associated with the transaction
    freeTransaction();
  }
}
  
////////////////////////////////////////////////////////////////////////////////
/// @brief return the names of all collections used in the transaction
////////////////////////////////////////////////////////////////////////////////

std::vector<std::string> Transaction::collectionNames() const {
  std::vector<std::string> result;

  for (size_t i = 0; i < _trx->_collections._length; ++i) {
    auto trxCollection = static_cast<TRI_transaction_collection_t*>(
        TRI_AtVectorPointer(&_trx->_collections, i));

    if (trxCollection->_collection != nullptr) {
      result.emplace_back(trxCollection->_collection->_name);
    }
  }

  return result;
}
  
////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection name resolver
////////////////////////////////////////////////////////////////////////////////

CollectionNameResolver const* Transaction::resolver() const {
  CollectionNameResolver const* r = this->_transactionContext->getResolver();
  TRI_ASSERT(r != nullptr);
  return r;
}
  
////////////////////////////////////////////////////////////////////////////////
/// @brief return the transaction collection for a document collection
////////////////////////////////////////////////////////////////////////////////

TRI_transaction_collection_t* Transaction::trxCollection(TRI_voc_cid_t cid) const {
  TRI_ASSERT(_trx != nullptr);
  TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);

  return TRI_GetCollectionTransaction(_trx, cid, TRI_TRANSACTION_READ);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief order a ditch for a collection
////////////////////////////////////////////////////////////////////////////////

DocumentDitch* Transaction::orderDitch(TRI_voc_cid_t cid) {
  TRI_ASSERT(_trx != nullptr);
  TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING ||
             getStatus() == TRI_TRANSACTION_CREATED);

  TRI_transaction_collection_t* trxCollection = TRI_GetCollectionTransaction(_trx, cid, TRI_TRANSACTION_READ);

  if (trxCollection == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);    
  }

  TRI_ASSERT(trxCollection->_collection != nullptr);

  TRI_document_collection_t* document =
      trxCollection->_collection->_collection;
  TRI_ASSERT(document != nullptr);

  DocumentDitch* ditch = _transactionContext->orderDitch(document);

  if (ditch == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }
  return ditch;
}
  
////////////////////////////////////////////////////////////////////////////////
/// @brief extract the _key attribute from a slice
////////////////////////////////////////////////////////////////////////////////

std::string Transaction::extractKey(VPackSlice const slice) {
  // extract _key
  if (slice.isObject()) {
    VPackSlice k = slice.get(TRI_VOC_ATTRIBUTE_KEY);
    if (!k.isString()) {
      return ""; // fail
    }
    return k.copyString();
  } 
  if (slice.isString()) {
    std::string key = slice.copyString();
    size_t pos = key.find('/');
    if (pos != std::string::npos) {
      key = key.substr(pos+1);
    }
    return key;
  } 
  return "";
}

//////////////////////////////////////////////////////////////////////////////
/// @brief extract the _id attribute from a slice, and convert it into a 
/// string
//////////////////////////////////////////////////////////////////////////////

std::string Transaction::extractIdString(VPackSlice const slice) {
  return extractIdString(resolver(), slice, VPackSlice());
}

//////////////////////////////////////////////////////////////////////////////
/// @brief extract the _id attribute from a slice, and convert it into a 
/// string, static method
//////////////////////////////////////////////////////////////////////////////

std::string Transaction::extractIdString(CollectionNameResolver const* resolver,
                                         VPackSlice const& slice,
                                         VPackSlice const& base) {
  VPackSlice id = slice;
  if (slice.isObject()) {
    // extract id attribute from object
    id = slice.get(TRI_VOC_ATTRIBUTE_ID);
  }
  if (id.isString()) {
    // already a string...
    return id.copyString();
  }
  if (!id.isCustom() || id.head() != 0xf3) {
    // invalid type for _id
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }

  // we now need to extract the _key attribute
  VPackSlice key;
  if (slice.isObject()) {
    key = slice.get(TRI_VOC_ATTRIBUTE_KEY);
  } else if (base.isObject()) {
    key = base.get(TRI_VOC_ATTRIBUTE_KEY);
  }

  if (!key.isString()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }
  
  uint64_t cid = DatafileHelper::ReadNumber<uint64_t>(id.begin() + 1, sizeof(uint64_t));
  char buffer[512];  // This is enough for collection name + _key
  size_t len = resolver->getCollectionNameCluster(&buffer[0], cid);
  buffer[len] = '/';

  VPackValueLength keyLength;
  char const* p = key.getString(keyLength);
  if (p == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "invalid _key value");
  }
  memcpy(&buffer[len + 1], p, keyLength);
  return std::string(&buffer[0], len + 1 + keyLength);
}
  
//////////////////////////////////////////////////////////////////////////////
/// @brief build a VPack object with _id, _key and _rev, the result is
/// added to the builder in the argument as a single object.
//////////////////////////////////////////////////////////////////////////////

void Transaction::buildDocumentIdentity(VPackBuilder& builder,
                                        TRI_voc_cid_t cid,
                                        std::string const& key,
                                        VPackSlice const rid,
                                        VPackSlice const oldRid,
                                        TRI_doc_mptr_t const* oldMptr,
                                        TRI_doc_mptr_t const* newMptr) {
  std::string collectionName = resolver()->getCollectionName(cid);

  builder.openObject();
  builder.add(TRI_VOC_ATTRIBUTE_ID, VPackValue(collectionName + "/" + key));
  builder.add(TRI_VOC_ATTRIBUTE_KEY, VPackValue(key));
  TRI_ASSERT(!rid.isNone());
  builder.add(TRI_VOC_ATTRIBUTE_REV, rid);
  if (!oldRid.isNone()) {
    builder.add("_oldRev", oldRid);
  }
  if (oldMptr != nullptr) {
    builder.add("old", VPackSlice(oldMptr->vpack()));
    // TODO: add externals later.
    //builder.add("old", VPackValue(VPackValueType::External, oldMptr->vpack()));
  }
  if (newMptr != nullptr) {
    builder.add("new", VPackSlice(newMptr->vpack()));
    // TODO: add externals later.
    //builder.add("new", VPackValue(VPackValueType::External, newMptr->vpack()));
  }
  builder.close();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief begin the transaction
////////////////////////////////////////////////////////////////////////////////

int Transaction::begin() {
  if (_trx == nullptr) {
    return TRI_ERROR_TRANSACTION_INTERNAL;
  }

  if (_setupState != TRI_ERROR_NO_ERROR) {
    return _setupState;
  }

  if (!_isReal) {
    if (_nestingLevel == 0) {
      _trx->_status = TRI_TRANSACTION_RUNNING;
    }
    return TRI_ERROR_NO_ERROR;
  }

  int res = TRI_BeginTransaction(_trx, _hints, _nestingLevel);

  return res;
}
  
////////////////////////////////////////////////////////////////////////////////
/// @brief commit / finish the transaction
////////////////////////////////////////////////////////////////////////////////

int Transaction::commit() {
  if (_trx == nullptr || getStatus() != TRI_TRANSACTION_RUNNING) {
    // transaction not created or not running
    return TRI_ERROR_TRANSACTION_INTERNAL;
  }

  if (!_isReal) {
    if (_nestingLevel == 0) {
      _trx->_status = TRI_TRANSACTION_COMMITTED;
    }
    return TRI_ERROR_NO_ERROR;
  }

  int res = TRI_CommitTransaction(_trx, _nestingLevel);

  return res;
}
  
////////////////////////////////////////////////////////////////////////////////
/// @brief abort the transaction
////////////////////////////////////////////////////////////////////////////////

int Transaction::abort() {
  if (_trx == nullptr || getStatus() != TRI_TRANSACTION_RUNNING) {
    // transaction not created or not running
    return TRI_ERROR_TRANSACTION_INTERNAL;
  }

  if (!_isReal) {
    if (_nestingLevel == 0) {
      _trx->_status = TRI_TRANSACTION_ABORTED;
    }

    return TRI_ERROR_NO_ERROR;
  }

  int res = TRI_AbortTransaction(_trx, _nestingLevel);

  return res;
}
  
////////////////////////////////////////////////////////////////////////////////
/// @brief finish a transaction (commit or abort), based on the previous state
////////////////////////////////////////////////////////////////////////////////

int Transaction::finish(int errorNum) {
  if (errorNum == TRI_ERROR_NO_ERROR) {
    // there was no previous error, so we'll commit
    return this->commit();
  }
  
  // there was a previous error, so we'll abort
  this->abort();

  // return original error number
  return errorNum;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief read any (random) document
////////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::any(std::string const& collectionName) {
  return any(collectionName, 0, 1);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief read all master pointers, using skip and limit.
/// The resualt guarantees that all documents are contained exactly once
/// as long as the collection is not modified.
////////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::any(std::string const& collectionName,
                                 uint64_t skip, uint64_t limit) {
  if (ServerState::instance()->isCoordinator()) {
    return anyCoordinator(collectionName, skip, limit);
  }
  return anyLocal(collectionName, skip, limit);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief fetches documents in a collection in random order, coordinator
////////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::anyCoordinator(std::string const&, uint64_t,
                                            uint64_t) {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief fetches documents in a collection in random order, local
////////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::anyLocal(std::string const& collectionName,
                                      uint64_t skip, uint64_t limit) {
  TRI_voc_cid_t cid = resolver()->getCollectionIdLocal(collectionName);

  if (cid == 0) {
    THROW_ARANGO_EXCEPTION_FORMAT(TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND, "'%s'",
                                  collectionName.c_str());
  }
 
  orderDitch(cid); // will throw when it fails 
  
  int res = lock(trxCollection(cid), TRI_TRANSACTION_READ);

  if (res != TRI_ERROR_NO_ERROR) {
    return OperationResult(res);
  }
  
  VPackBuilder resultBuilder;
  resultBuilder.openArray();

  std::shared_ptr<OperationCursor> cursor =
      indexScan(collectionName, Transaction::CursorType::ANY, IndexHandle(), 
                {}, skip, limit, 1000, false);

  auto result = std::make_shared<OperationResult>(TRI_ERROR_NO_ERROR);
  while (cursor->hasMore()) {
    cursor->getMore(result);

    if (result->failed()) {
      return OperationResult(result->code);
    }
  
    VPackSlice docs = result->slice();
    VPackArrayIterator it(docs);
    while (it.valid()) {
      resultBuilder.add(it.value());
      it.next();
    }
  }

  resultBuilder.close();

  res = unlock(trxCollection(cid), TRI_TRANSACTION_READ);

  if (res != TRI_ERROR_NO_ERROR) {
    return OperationResult(res);
  }

  return OperationResult(resultBuilder.steal(),
                         transactionContext()->orderCustomTypeHandler(), "",
                         TRI_ERROR_NO_ERROR, false);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief add a collection to the transaction for read, at runtime
//////////////////////////////////////////////////////////////////////////////

TRI_voc_cid_t Transaction::addCollectionAtRuntime(std::string const& collectionName) {
  auto cid = resolver()->getCollectionIdLocal(collectionName);

  if (cid == 0) {
    THROW_ARANGO_EXCEPTION_FORMAT(TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND, "'%s'",
                                  collectionName.c_str());
  }
  return addCollectionAtRuntime(cid, collectionName);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief return the type of a collection
//////////////////////////////////////////////////////////////////////////////

bool Transaction::isEdgeCollection(std::string const& collectionName) {
  return getCollectionType(collectionName) == TRI_COL_TYPE_EDGE;
}

//////////////////////////////////////////////////////////////////////////////
/// @brief return the type of a collection
//////////////////////////////////////////////////////////////////////////////

bool Transaction::isDocumentCollection(std::string const& collectionName) {
  return getCollectionType(collectionName) == TRI_COL_TYPE_DOCUMENT;
}

//////////////////////////////////////////////////////////////////////////////
/// @brief return the type of a collection
//////////////////////////////////////////////////////////////////////////////
  
TRI_col_type_t Transaction::getCollectionType(std::string const& collectionName) {
  if (ServerState::instance()->isCoordinator()) {
    return resolver()->getCollectionTypeCluster(collectionName);
  }
  return resolver()->getCollectionType(collectionName);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief return the name of a collection
//////////////////////////////////////////////////////////////////////////////
  
std::string Transaction::collectionName(TRI_voc_cid_t cid) { 
  return resolver()->getCollectionName(cid);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief return the edge index handle of collection
//////////////////////////////////////////////////////////////////////////////

Transaction::IndexHandle Transaction::edgeIndexHandle(std::string const& collectionName) {
  if (!isEdgeCollection(collectionName)) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID);
  }
  auto indexes = indexesForCollection(collectionName); 
  for (auto idx : indexes) {
    if (idx->type() == Index::TRI_IDX_TYPE_EDGE_INDEX) {
      return IndexHandle(idx);
    }
  }
  THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief Iterate over all elements of the collection.
//////////////////////////////////////////////////////////////////////////////

void Transaction::invokeOnAllElements(std::string const& collectionName,
                                      std::function<bool(TRI_doc_mptr_t const*)> callback) {
  TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);
  if (ServerState::instance()->isCoordinator()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }
  
  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName); 
  TRI_transaction_collection_t* trxCol = trxCollection(cid);
  TRI_document_collection_t* document = documentCollection(trxCol);

  orderDitch(cid); // will throw when it fails

  int res = lock(trxCol, TRI_TRANSACTION_READ);
  
  if (res != TRI_ERROR_NO_ERROR) {
    THROW_ARANGO_EXCEPTION(res);
  }

  auto primaryIndex = document->primaryIndex();
  primaryIndex->invokeOnAllElements(callback);
  
  res = unlock(trxCol, TRI_TRANSACTION_READ);
  
  if (res != TRI_ERROR_NO_ERROR) {
    THROW_ARANGO_EXCEPTION(res);
  }
}

//////////////////////////////////////////////////////////////////////////////
/// @brief return one or multiple documents from a collection
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::document(std::string const& collectionName,
                                      VPackSlice const value,
                                      OperationOptions& options) {
  TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);

  if (!value.isObject() && !value.isArray()) {
    // must provide a document object or an array of documents
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }

  if (ServerState::instance()->isCoordinator()) {
    return documentCoordinator(collectionName, value, options);
  }

  return documentLocal(collectionName, value, options);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief read one or multiple documents in a collection, coordinator
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::documentCoordinator(std::string const& collectionName,
                                                 VPackSlice const value,
                                                 OperationOptions& options) {
  if (value.isArray()) {
    // multi-document variant is not yet implemented
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  auto headers = std::make_unique<std::map<std::string, std::string>>();
  GeneralResponse::ResponseCode responseCode;
  std::map<std::string, std::string> resultHeaders;
  std::string resultBody;

  std::string key(Transaction::extractKey(value));
  if (key.empty()) {
    return OperationResult(TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD);
  }
  TRI_voc_rid_t expectedRevision = TRI_ExtractRevisionId(value);

  int res = arangodb::getDocumentOnCoordinator(
      _vocbase->_name, collectionName, key, expectedRevision, headers, true,
      responseCode, resultHeaders, resultBody);

  if (res == TRI_ERROR_NO_ERROR) {
    if (responseCode == GeneralResponse::ResponseCode::OK ||
        responseCode == GeneralResponse::ResponseCode::PRECONDITION_FAILED) {
      VPackParser parser;
      try {
        parser.parse(resultBody);
        auto bui = parser.steal();
        auto buf = bui->steal();
        return OperationResult(buf, nullptr, "", 
            responseCode == GeneralResponse::ResponseCode::OK ?
            TRI_ERROR_NO_ERROR : TRI_ERROR_ARANGO_CONFLICT, false);
      }
      catch (VPackException& e) {
        std::string message = "JSON from DBserver not parseable: "
                              + resultBody + ":" + e.what();
        return OperationResult(TRI_ERROR_INTERNAL, message);
      }
    } else if (responseCode == GeneralResponse::ResponseCode::NOT_FOUND) {
      return OperationResult(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND);
    } else {
      return OperationResult(TRI_ERROR_INTERNAL);
    }
  }
  return OperationResult(res);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief read one or multiple documents in a collection, local
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::documentLocal(std::string const& collectionName,
                                           VPackSlice const value,
                                           OperationOptions& options) {
  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName); 
  TRI_document_collection_t* document = documentCollection(trxCollection(cid));

  orderDitch(cid); // will throw when it fails
 
  VPackBuilder resultBuilder;

  auto workOnOneDocument = [&](VPackSlice const value) -> int {
    std::string key(Transaction::extractKey(value));
    if (key.empty()) {
      return TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD;
    }

    VPackSlice expectedRevision;
    if (!options.ignoreRevs) {
      expectedRevision = TRI_ExtractRevisionIdAsSlice(value);
    }

    TRI_doc_mptr_t mptr;
    int res = document->read(this, key, &mptr, !isLocked(document, TRI_TRANSACTION_READ));

    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }
  
    TRI_ASSERT(mptr.getDataPtr() != nullptr);
    if (!expectedRevision.isNone()) {
      VPackSlice foundRevision = mptr.revisionIdAsSlice();
      if (expectedRevision != foundRevision) {
        // still return 
        buildDocumentIdentity(resultBuilder, cid, key, foundRevision, 
                              VPackSlice(), nullptr, nullptr);
        return TRI_ERROR_ARANGO_CONFLICT;
      }
    }
  
    if (!options.silent) {
      //resultBuilder.add(VPackValue(static_cast<void const*>(mptr.vpack()), VPackValueType::External));
      // This is the future, for now, we have to do this:
      resultBuilder.add(VPackSlice(mptr.vpack()));
    }

    return TRI_ERROR_NO_ERROR;
  };

  int res = TRI_ERROR_NO_ERROR;
  if (!value.isArray()) {
    res = workOnOneDocument(value);
  } else {
    VPackArrayBuilder guard(&resultBuilder);
    for (auto const& s : VPackArrayIterator(value)) {
      res = workOnOneDocument(s);
      if (res != TRI_ERROR_NO_ERROR) {
        break;
      }
    }
  }

  return OperationResult(resultBuilder.steal(), 
                         transactionContext()->orderCustomTypeHandler(), "",
                         res, options.waitForSync); 
}

//////////////////////////////////////////////////////////////////////////////
/// @brief create one or multiple documents in a collection
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::insert(std::string const& collectionName,
                                    VPackSlice const value,
                                    OperationOptions const& options) {
  TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);

  if (!value.isObject() && !value.isArray()) {
    // must provide a document object or an array of documents
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }

  // Validate Edges
  OperationOptions optionsCopy = options;

  if (ServerState::instance()->isCoordinator()) {
    return insertCoordinator(collectionName, value, optionsCopy);
  }

  return insertLocal(collectionName, value, optionsCopy);
}
   
//////////////////////////////////////////////////////////////////////////////
/// @brief create one or multiple documents in a collection, coordinator
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::insertCoordinator(std::string const& collectionName,
                                               VPackSlice const value,
                                               OperationOptions& options) {

  std::map<std::string, std::string> headers;
  GeneralResponse::ResponseCode responseCode;

  std::unordered_map<int, size_t> errorCounter;
  auto resultBody = std::make_shared<VPackBuilder>();

  int res = arangodb::createDocumentOnCoordinator(
      _vocbase->_name, collectionName, options, value, headers, responseCode,
      errorCounter, resultBody);

  if (res == TRI_ERROR_NO_ERROR) {
    if (responseCode == GeneralResponse::ResponseCode::ACCEPTED ||
        responseCode == GeneralResponse::ResponseCode::CREATED) {
      return OperationResult(resultBody->steal(), nullptr, "", TRI_ERROR_NO_ERROR, responseCode == GeneralResponse::ResponseCode::CREATED);
    } else if (responseCode == GeneralResponse::ResponseCode::PRECONDITION_FAILED) {
      return OperationResult(TRI_ERROR_ARANGO_CONFLICT);
    } else if (responseCode == GeneralResponse::ResponseCode::BAD) {
      return DBServerResponseBad(resultBody);
    } else if (responseCode == GeneralResponse::ResponseCode::NOT_FOUND) {
      return OperationResult(TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND);
    } else if (responseCode == GeneralResponse::ResponseCode::CONFLICT) {
      return OperationResult(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED);
    } else {
      return OperationResult(TRI_ERROR_INTERNAL);
    }
  }
  return OperationResult(res);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief create one or multiple documents in a collection, local
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::insertLocal(std::string const& collectionName,
                                         VPackSlice const value,
                                         OperationOptions& options) {
 
  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName); 
  TRI_document_collection_t* document = documentCollection(trxCollection(cid));

  VPackBuilder resultBuilder;

  auto workForOneDocument = [&](VPackSlice const value) -> int {
    if (!value.isObject()) {
      return TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID;
    }
    TRI_doc_mptr_t mptr;
    int res = document->insert(this, value, &mptr, options,
        !isLocked(document, TRI_TRANSACTION_WRITE));
    
    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }

    if (options.silent) {
      // no need to construct the result object
      return TRI_ERROR_NO_ERROR;
    }

    TRI_ASSERT(mptr.getDataPtr() != nullptr);
    
    std::string keyString 
        = VPackSlice(mptr.vpack()).get(TRI_VOC_ATTRIBUTE_KEY).copyString();

    buildDocumentIdentity(resultBuilder, cid, keyString, 
        mptr.revisionIdAsSlice(), VPackSlice(),
        nullptr, options.returnNew ? &mptr : nullptr);

    return TRI_ERROR_NO_ERROR;
  };

  int res = TRI_ERROR_NO_ERROR;
  std::unordered_map<int, size_t> countErrorCodes;
  if (value.isArray()) {
    VPackArrayBuilder b(&resultBuilder);
    for (auto const& s : VPackArrayIterator(value)) {
      res = workForOneDocument(s);
      if (res != TRI_ERROR_NO_ERROR) {
        createBabiesError(resultBuilder, countErrorCodes, res);
      }
    }
    // With babies the reporting is handled somewhere else.
    res = TRI_ERROR_NO_ERROR;
  } else {
    res = workForOneDocument(value);
  }

  if (ServerState::instance()->isDBServer()) {
    // Now replicate the same operation on all followers:
    auto const& followerInfo = document->followers();
    std::shared_ptr<std::vector<ServerID> const> followers = followerInfo->get();
    if (followers->size() > 0) {
      auto cc = arangodb::ClusterComm::instance();

      std::string path
          = "/_db/" +
            arangodb::basics::StringUtils::urlEncode(_vocbase->_name) +
            "/_api/document/" +
            arangodb::basics::StringUtils::urlEncode(document->_info.name());
      // FIXME: scan options and append the right ones

      // FIXME: We might only want to send the successful tries to the
      // replica, for now, we simply send the same body:
      auto body = std::make_shared<std::string>();
      *body = std::move(value.toJson());

      // Now prepare the requests:
      std::vector<ClusterCommRequest> requests;
      for (auto const& f : *followers) {
        requests.emplace_back("server:" + f, 
                              arangodb::GeneralRequest::RequestType::POST,
                              path, body);
      }
      size_t nrDone = 0;
      size_t nrGood = cc->performRequests(requests, 60.0, nrDone,
                                          Logger::REPLICATION);
      if (nrGood < followers->size()) {
        // we drop all followers that were not successful:
        for (size_t i = 0; i < followers->size(); ++i) {
          if (!requests[i].done || 
              requests[i].result.status != CL_COMM_RECEIVED ||
              (requests[i].result.answer_code != 
                   GeneralResponse::ResponseCode::ACCEPTED &&
               requests[i].result.answer_code != 
                   GeneralResponse::ResponseCode::CREATED)) {
            followerInfo->remove(requests[i].result.serverID);
            LOG_TOPIC(ERR, Logger::REPLICATION)
                << "insertLocal: dropping follower "
                << requests[i].result.serverID;
          }
        }
      }
    }
  }
  
  return OperationResult(resultBuilder.steal(), nullptr, "", res,
                         options.waitForSync, countErrorCodes);
}
  
//////////////////////////////////////////////////////////////////////////////
/// @brief update/patch one or multiple documents in a collection
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::update(std::string const& collectionName,
                                    VPackSlice const newValue,
                                    OperationOptions const& options) {
  TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);

  if (!newValue.isObject() && !newValue.isArray()) {
    // must provide a document object or an array of documents
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }

  OperationOptions optionsCopy = options;

  if (ServerState::instance()->isCoordinator()) {
    return updateCoordinator(collectionName, newValue, optionsCopy);
  }

  return modifyLocal(collectionName, newValue, optionsCopy,
                     TRI_VOC_DOCUMENT_OPERATION_UPDATE);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief update one or multiple documents in a collection, coordinator
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::updateCoordinator(std::string const& collectionName,
                                               VPackSlice const newValue,
                                               OperationOptions& options) {

  if (newValue.isArray()) {
    // multi-document variant is not yet implemented
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }
  
  auto headers = std::make_unique<std::map<std::string, std::string>>();
  GeneralResponse::ResponseCode responseCode;
  std::map<std::string, std::string> resultHeaders;
  std::string resultBody;

  std::string key(Transaction::extractKey(newValue));
  if (key.empty()) {
    return OperationResult(TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD);
  }
  TRI_voc_rid_t const expectedRevision 
      = options.ignoreRevs ? 0 : TRI_ExtractRevisionId(newValue);

  int res = arangodb::modifyDocumentOnCoordinator(
      _vocbase->_name, collectionName, key, expectedRevision, options,
      true /* isPatch */, newValue, headers, responseCode, resultHeaders,
      resultBody);

  if (res == TRI_ERROR_NO_ERROR) {
    int errorCode = TRI_ERROR_NO_ERROR;
    switch (responseCode) {
      case GeneralResponse::ResponseCode::CONFLICT:
        errorCode = TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED;
        // Fall through
      case GeneralResponse::ResponseCode::PRECONDITION_FAILED:
        if (errorCode == TRI_ERROR_NO_ERROR) {
          errorCode = TRI_ERROR_ARANGO_CONFLICT;
        }
        // Fall through
      case GeneralResponse::ResponseCode::ACCEPTED:
      case GeneralResponse::ResponseCode::CREATED:
        {
          VPackParser parser;
          try {
            parser.parse(resultBody);
            auto bui = parser.steal();
            auto buf = bui->steal();
            return OperationResult(
                buf, nullptr, "", errorCode,
                responseCode == GeneralResponse::ResponseCode::CREATED);
          }
          catch (VPackException& e) {
            std::string message = "JSON from DBserver not parseable: "
                                  + resultBody + ":" + e.what();
            return OperationResult(TRI_ERROR_INTERNAL, message);
          }
        }
      case GeneralResponse::ResponseCode::BAD:
        return DBServerResponseBad(resultBody);
      case GeneralResponse::ResponseCode::NOT_FOUND:
        return OperationResult(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND);
      default:
        return OperationResult(TRI_ERROR_INTERNAL);
    }
  }
  return OperationResult(res);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief replace one or multiple documents in a collection
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::replace(std::string const& collectionName,
                                     VPackSlice const newValue,
                                     OperationOptions const& options) {
  TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);

  if (!newValue.isObject() && !newValue.isArray()) {
    // must provide a document object or an array of documents
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }

  OperationOptions optionsCopy = options;

  if (ServerState::instance()->isCoordinator()) {
    return replaceCoordinator(collectionName, newValue, optionsCopy);
  }

  return modifyLocal(collectionName, newValue, optionsCopy,
                     TRI_VOC_DOCUMENT_OPERATION_REPLACE);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief replace one or multiple documents in a collection, coordinator
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::replaceCoordinator(std::string const& collectionName,
                                                VPackSlice const newValue,
                                                OperationOptions& options) {
  if (newValue.isArray()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }

  auto headers = std::make_unique<std::map<std::string, std::string>>();
  GeneralResponse::ResponseCode responseCode;
  std::map<std::string, std::string> resultHeaders;
  std::string resultBody;

  std::string key(Transaction::extractKey(newValue));
  if (key.empty()) {
    return OperationResult(TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD);
  }
  TRI_voc_rid_t const expectedRevision 
      = options.ignoreRevs ? 0 : TRI_ExtractRevisionId(newValue);

  int res = arangodb::modifyDocumentOnCoordinator(
      _vocbase->_name, collectionName, key, expectedRevision, options,
      false /* isPatch */, newValue, headers, responseCode, resultHeaders,
      resultBody);

  if (res == TRI_ERROR_NO_ERROR) {
    int errorCode = TRI_ERROR_NO_ERROR;
    switch (responseCode) {
      case GeneralResponse::ResponseCode::CONFLICT:
        errorCode = TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED;
      case GeneralResponse::ResponseCode::PRECONDITION_FAILED:
        if (errorCode == TRI_ERROR_NO_ERROR) {
          errorCode = TRI_ERROR_ARANGO_CONFLICT;
        }
        // Fall through
      case GeneralResponse::ResponseCode::ACCEPTED:
      case GeneralResponse::ResponseCode::CREATED:
        {
          VPackParser parser;
          try {
            parser.parse(resultBody);
            auto bui = parser.steal();
            auto buf = bui->steal();
            return OperationResult(
                buf, nullptr, "", errorCode,
                responseCode == GeneralResponse::ResponseCode::CREATED);
          }
          catch (VPackException& e) {
            std::string message = "JSON from DBserver not parseable: "
                                  + resultBody + ":" + e.what();
            return OperationResult(TRI_ERROR_INTERNAL, message);
          }
        }
      case GeneralResponse::ResponseCode::BAD:
        return DBServerResponseBad(resultBody);
      case GeneralResponse::ResponseCode::NOT_FOUND:
        return OperationResult(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND);
      default:
        return OperationResult(TRI_ERROR_INTERNAL);
    }
  }
  return OperationResult(res);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief replace one or multiple documents in a collection, local
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::modifyLocal(
    std::string const& collectionName,
    VPackSlice const newValue,
    OperationOptions& options,
    TRI_voc_document_operation_e operation) {

  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName); 
  TRI_document_collection_t* document = documentCollection(trxCollection(cid));

  // Update/replace are a read and a write, let's get the write lock already
  // for the read operation:
  int res = lock(trxCollection(cid), TRI_TRANSACTION_WRITE);

  if (res != TRI_ERROR_NO_ERROR) {
    return OperationResult(res);
  }

  VPackBuilder resultBuilder;  // building the complete result

  auto workForOneDocument = [&](VPackSlice const newVal) -> int {
    if (!newVal.isObject()) {
      return TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID;
    }
    TRI_doc_mptr_t mptr;
    VPackSlice actualRevision;
    TRI_doc_mptr_t previous;

    if (operation == TRI_VOC_DOCUMENT_OPERATION_REPLACE) {
      res = document->replace(this, newVal, &mptr, options,
          !isLocked(document, TRI_TRANSACTION_WRITE), actualRevision,
          previous);
    } else {
      res = document->update(this, newVal, &mptr, options,
          !isLocked(document, TRI_TRANSACTION_WRITE), actualRevision,
          previous);
    }

    if (res == TRI_ERROR_ARANGO_CONFLICT) {
      // still return 
      if (!options.silent) {
        std::string key = newVal.get(TRI_VOC_ATTRIBUTE_KEY).copyString();
        buildDocumentIdentity(resultBuilder, cid, key, actualRevision,
                              VPackSlice(), 
                              options.returnOld ? &previous : nullptr, nullptr);
      }
      return TRI_ERROR_ARANGO_CONFLICT;
    } else if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }

    TRI_ASSERT(mptr.getDataPtr() != nullptr);

    if (!options.silent) {
      std::string key = newVal.get(TRI_VOC_ATTRIBUTE_KEY).copyString();
      buildDocumentIdentity(resultBuilder, cid, key, 
          mptr.revisionIdAsSlice(), actualRevision, 
          options.returnOld ? &previous : nullptr , 
          options.returnNew ? &mptr : nullptr);
    }
    return TRI_ERROR_NO_ERROR;
  };

  res = TRI_ERROR_NO_ERROR;

  if (newValue.isArray()) {
    VPackArrayBuilder b(&resultBuilder);
    VPackArrayIterator it(newValue);
    while (it.valid()) {
      res = workForOneDocument(it.value());
      if (res != TRI_ERROR_NO_ERROR) {
        break;
      }
      ++it;
    }
  } else {
    res = workForOneDocument(newValue);
  }
  return OperationResult(resultBuilder.steal(), nullptr, "", res,
                         options.waitForSync); 
}

//////////////////////////////////////////////////////////////////////////////
/// @brief remove one or multiple documents in a collection
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::remove(std::string const& collectionName,
                                    VPackSlice const value,
                                    OperationOptions const& options) {
  TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);

  if (!value.isObject() && !value.isArray() && !value.isString()) {
    // must provide a document object or an array of documents
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }

  OperationOptions optionsCopy = options;

  if (ServerState::instance()->isCoordinator()) {
    return removeCoordinator(collectionName, value, optionsCopy);
  }

  return removeLocal(collectionName, value, optionsCopy);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief remove one or multiple documents in a collection, coordinator
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::removeCoordinator(std::string const& collectionName,
                                               VPackSlice const value,
                                               OperationOptions& options) {

  if (value.isArray()) {
    // multi-document variant is not yet implemented
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }
  
  auto headers = std::make_unique<std::map<std::string, std::string>>();
  GeneralResponse::ResponseCode responseCode;
  std::map<std::string, std::string> resultHeaders;
  std::string resultBody;

  std::string key(Transaction::extractKey(value));
  if (key.empty()) {
    return OperationResult(TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD);
  }
  TRI_voc_rid_t expectedRevision = 0;
  if (!options.ignoreRevs) {
    expectedRevision = TRI_ExtractRevisionId(value);
  }

  int res = arangodb::deleteDocumentOnCoordinator(
      _vocbase->_name, collectionName, key, expectedRevision, options, headers,
      responseCode, resultHeaders, resultBody);

  if (res == TRI_ERROR_NO_ERROR) {
    if (responseCode == GeneralResponse::ResponseCode::OK ||
        responseCode == GeneralResponse::ResponseCode::ACCEPTED ||
        responseCode == GeneralResponse::ResponseCode::PRECONDITION_FAILED) {
      VPackParser parser;
      try {
        parser.parse(resultBody);
        auto bui = parser.steal();
        auto buf = bui->steal();
        return OperationResult(
            buf, nullptr, "",
            responseCode == GeneralResponse::ResponseCode::PRECONDITION_FAILED
                ? TRI_ERROR_ARANGO_CONFLICT
                : TRI_ERROR_NO_ERROR,
            responseCode != GeneralResponse::ResponseCode::ACCEPTED);
      }
      catch (VPackException& e) {
        std::string message = "JSON from DBserver not parseable: "
                              + resultBody + ":" + e.what();
        return OperationResult(TRI_ERROR_INTERNAL, message);
      }
    } else if (responseCode == GeneralResponse::ResponseCode::BAD) {
      return DBServerResponseBad(resultBody);
    } else if (responseCode == GeneralResponse::ResponseCode::NOT_FOUND) {
      return OperationResult(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND);
    } else {
      return OperationResult(TRI_ERROR_INTERNAL);
    }
  }
  return OperationResult(res);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief remove one or multiple documents in a collection, local
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::removeLocal(std::string const& collectionName,
                                         VPackSlice const value,
                                         OperationOptions& options) {
  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName); 
  TRI_document_collection_t* document = documentCollection(trxCollection(cid));
 
  VPackBuilder resultBuilder;

  auto workOnOneDocument = [&](VPackSlice value) -> int {
    VPackSlice actualRevision;
    TRI_doc_mptr_t previous;
    std::string key;
    std::shared_ptr<VPackBuilder> builder;
    if (value.isString()) {
      key = value.copyString();
      size_t pos = key.find('/');
      if (pos != std::string::npos) {
        key = key.substr(pos+1);
        builder = std::make_shared<VPackBuilder>();
        builder->add(VPackValue(key));
        value = builder->slice();
      }
    } else if (value.isObject()) {
      key = value.get(TRI_VOC_ATTRIBUTE_KEY).copyString();
    } else {
      return TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD;
    }

    int res = document->remove(this, value, options,
                               !isLocked(document, TRI_TRANSACTION_WRITE),
                               actualRevision, previous);
    
    if (res != TRI_ERROR_NO_ERROR) {
      if (res == TRI_ERROR_ARANGO_CONFLICT && !options.silent) {
        buildDocumentIdentity(resultBuilder, cid, key,
                              actualRevision, VPackSlice(), 
                              options.returnOld ? &previous : nullptr, nullptr);
      }
      return res;
    }

    if (!options.silent) {
      buildDocumentIdentity(resultBuilder, cid, key,
                            actualRevision, VPackSlice(),
                            options.returnOld ? &previous : nullptr, nullptr);
    }

    return TRI_ERROR_NO_ERROR;
  };

  int res = TRI_ERROR_NO_ERROR;
  if (value.isArray()) {
    VPackArrayBuilder guard(&resultBuilder);
    for (auto const& s : VPackArrayIterator(value)) {
      res = workOnOneDocument(s);
      if (res != TRI_ERROR_NO_ERROR) {
        break;
      }
    }
  } else {
    res = workOnOneDocument(value);
  }
  return OperationResult(resultBuilder.steal(), nullptr, "", res,
                         options.waitForSync); 
}

////////////////////////////////////////////////////////////////////////////////
/// @brief fetches all document keys in a collection
////////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::allKeys(std::string const& collectionName,
                                     std::string const& type,
                                     OperationOptions const& options) {
  TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);
  
  std::string prefix;

  std::string realCollName = resolver()->getCollectionName(collectionName);

  if (type == "key") {
    prefix = "";
  } else if (type == "id") {
    prefix = realCollName + "/";
  } else {
    prefix = std::string("/_db/") + _vocbase->_name + "/_api/document/" + realCollName + "/";
  }
  
  OperationOptions optionsCopy = options;

  if (ServerState::instance()->isCoordinator()) {
    return allKeysCoordinator(collectionName, type, prefix, optionsCopy);
  }

  return allKeysLocal(collectionName, type, prefix, optionsCopy);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief fetches all document keys in a collection, coordinator
////////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::allKeysCoordinator(std::string const& collectionName,
                                                std::string const& type,
                                                std::string const& prefix,
                                                OperationOptions& options) {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief fetches all document keys in a collection, local
////////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::allKeysLocal(std::string const& collectionName,
                                          std::string const& type,
                                          std::string const& prefix,
                                          OperationOptions& options) {
  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName); 
  
  orderDitch(cid); // will throw when it fails
  
  int res = lock(trxCollection(cid), TRI_TRANSACTION_READ);

  if (res != TRI_ERROR_NO_ERROR) {
    return OperationResult(res);
  }
  
  VPackBuilder resultBuilder;
  resultBuilder.add(VPackValue(VPackValueType::Object));
  resultBuilder.add("documents", VPackValue(VPackValueType::Array));

  std::shared_ptr<OperationCursor> cursor =
      indexScan(collectionName, Transaction::CursorType::ALL, IndexHandle(),
                {}, 0, UINT64_MAX, 1000, false);

  auto result = std::make_shared<OperationResult>(TRI_ERROR_NO_ERROR);
  while (cursor->hasMore()) {
    cursor->getMore(result);

    if (result->failed()) {
      return OperationResult(result->code);
    }
  
    std::string value;
    VPackSlice docs = result->slice();
    VPackArrayIterator it(docs);
    while (it.valid()) {
      value.assign(prefix);
      value.append(it.value().get(TRI_VOC_ATTRIBUTE_KEY).copyString());
      resultBuilder.add(VPackValue(value));
      it.next();
    }
  }

  resultBuilder.close(); // array
  resultBuilder.close(); // object

  res = unlock(trxCollection(cid), TRI_TRANSACTION_READ);

  if (res != TRI_ERROR_NO_ERROR) {
    return OperationResult(res);
  }

  return OperationResult(resultBuilder.steal(),
                         transactionContext()->orderCustomTypeHandler(), "",
                         TRI_ERROR_NO_ERROR, false);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief fetches all documents in a collection
////////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::all(std::string const& collectionName,
                                 uint64_t skip, uint64_t limit,
                                 OperationOptions const& options) {
  TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);
  
  OperationOptions optionsCopy = options;

  if (ServerState::instance()->isCoordinator()) {
    return allCoordinator(collectionName, skip, limit, optionsCopy);
  }

  return allLocal(collectionName, skip, limit, optionsCopy);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief fetches all documents in a collection, coordinator
////////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::allCoordinator(std::string const& collectionName,
                                            uint64_t skip, uint64_t limit, 
                                            OperationOptions& options) {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief fetches all documents in a collection, local
////////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::allLocal(std::string const& collectionName,
                                      uint64_t skip, uint64_t limit,
                                      OperationOptions& options) {
  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName); 
  
  orderDitch(cid); // will throw when it fails
  
  int res = lock(trxCollection(cid), TRI_TRANSACTION_READ);

  if (res != TRI_ERROR_NO_ERROR) {
    return OperationResult(res);
  }
  
  VPackBuilder resultBuilder;
  resultBuilder.openArray();

  std::shared_ptr<OperationCursor> cursor =
      indexScan(collectionName, Transaction::CursorType::ALL, IndexHandle(),
                {}, skip, limit, 1000, false);

  auto result = std::make_shared<OperationResult>(TRI_ERROR_NO_ERROR);
  while (cursor->hasMore()) {
    cursor->getMore(result);

    if (result->failed()) {
      return OperationResult(result->code);
    }
  
    VPackSlice docs = result->slice();
    VPackArrayIterator it(docs);
    while (it.valid()) {
      resultBuilder.add(it.value());
      it.next();
    }
  }

  resultBuilder.close();

  res = unlock(trxCollection(cid), TRI_TRANSACTION_READ);

  if (res != TRI_ERROR_NO_ERROR) {
    return OperationResult(res);
  }

  return OperationResult(resultBuilder.steal(),
                         transactionContext()->orderCustomTypeHandler(), "",
                         TRI_ERROR_NO_ERROR, false);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief remove all documents in a collection
////////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::truncate(std::string const& collectionName,
                                      OperationOptions const& options) {
  TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);
  
  OperationOptions optionsCopy = options;

  if (ServerState::instance()->isCoordinator()) {
    return truncateCoordinator(collectionName, optionsCopy);
  }

  return truncateLocal(collectionName, optionsCopy);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief remove all documents in a collection, coordinator
////////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::truncateCoordinator(std::string const& collectionName,
                                                 OperationOptions& options) {
  return OperationResult(
      arangodb::truncateCollectionOnCoordinator(_vocbase->_name,
                                                collectionName));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief remove all documents in a collection, local
////////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::truncateLocal(std::string const& collectionName,
                                           OperationOptions& options) {
  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName); 
  
  orderDitch(cid); // will throw when it fails
  
  int res = lock(trxCollection(cid), TRI_TRANSACTION_WRITE);

  if (res != TRI_ERROR_NO_ERROR) {
    return OperationResult(res);
  }
 
  TRI_document_collection_t* document = documentCollection(trxCollection(cid));
  
  VPackBuilder keyBuilder;
  auto primaryIndex = document->primaryIndex();

  options.ignoreRevs = true;

  auto callback = [&](TRI_doc_mptr_t const* mptr) {
    VPackSlice actualRevision;
    TRI_doc_mptr_t previous;
    int res = document->remove(this, VPackSlice(mptr->vpack()), options, false,
                               actualRevision, previous);

    if (res != TRI_ERROR_NO_ERROR) {
      THROW_ARANGO_EXCEPTION(res);
    }

    return true;
  };

  try {
    primaryIndex->invokeOnAllElementsForRemoval(callback);
  }
  catch (basics::Exception const& ex) {
    unlock(trxCollection(cid), TRI_TRANSACTION_WRITE);
    return OperationResult(ex.code());
  }
  
  res = unlock(trxCollection(cid), TRI_TRANSACTION_WRITE);

  if (res != TRI_ERROR_NO_ERROR) {
    return OperationResult(res);
  }

  return OperationResult(TRI_ERROR_NO_ERROR);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief count the number of documents in a collection
////////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::count(std::string const& collectionName) {
  TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);

  if (ServerState::instance()->isCoordinator()) {
    return countCoordinator(collectionName);
  }

  return countLocal(collectionName);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief count the number of documents in a collection
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::countCoordinator(std::string const& collectionName) {
  uint64_t count = 0;
  int res = arangodb::countOnCoordinator(_vocbase->_name, collectionName, count);

  if (res != TRI_ERROR_NO_ERROR) {
    return OperationResult(res);
  }

  VPackBuilder resultBuilder;
  resultBuilder.add(VPackValue(count));

  return OperationResult(resultBuilder.steal(), nullptr, "", TRI_ERROR_NO_ERROR, false);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief count the number of documents in a collection
//////////////////////////////////////////////////////////////////////////////

OperationResult Transaction::countLocal(std::string const& collectionName) {
  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName); 
  
  int res = lock(trxCollection(cid), TRI_TRANSACTION_READ);

  if (res != TRI_ERROR_NO_ERROR) {
    return OperationResult(res);
  }
 
  TRI_document_collection_t* document = documentCollection(trxCollection(cid));

  VPackBuilder resultBuilder;
  resultBuilder.add(VPackValue(document->size()));

  res = unlock(trxCollection(cid), TRI_TRANSACTION_READ);
  
  if (res != TRI_ERROR_NO_ERROR) {
    return OperationResult(res);
  }

  return OperationResult(resultBuilder.steal(), nullptr, "", TRI_ERROR_NO_ERROR, false);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief Gets the best fitting index for an AQL condition.
/// note: the caller must have read-locked the underlying collection when
/// calling this method
//////////////////////////////////////////////////////////////////////////////

std::pair<bool, bool> Transaction::getBestIndexHandlesForFilterCondition(
    std::string const& collectionName, arangodb::aql::Ast* ast,
    arangodb::aql::AstNode* root,
    arangodb::aql::Variable const* reference,
    arangodb::aql::SortCondition const* sortCondition,
    size_t itemsInCollection,
    std::vector<IndexHandle>& usedIndexes,
    bool& isSorted) {
  // We can only start after DNF transformation
  TRI_ASSERT(root->type ==
             arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_NARY_OR);
  auto indexes = indexesForCollection(collectionName);

  bool canUseForFilter = (root->numMembers() > 0);
  bool canUseForSort = false;
  bool isSparse = false;

  for (size_t i = 0; i < root->numMembers(); ++i) {
    auto node = root->getMemberUnchecked(i);
    arangodb::aql::AstNode* specializedCondition = nullptr;
    auto canUseIndex = findIndexHandleForAndNode(
        indexes, node, reference, sortCondition, itemsInCollection, usedIndexes,
        specializedCondition, isSparse);

    if (canUseIndex.second && !canUseIndex.first) {
      // index can be used for sorting only
      // we need to abort further searching and only return one index
      TRI_ASSERT(!usedIndexes.empty());
      if (usedIndexes.size() > 1) {
        auto sortIndex = usedIndexes.back();

        usedIndexes.clear();
        usedIndexes.emplace_back(sortIndex);
      }

      TRI_ASSERT(usedIndexes.size() == 1);

      if (isSparse) {
        // cannot use a sparse index for sorting alone
        usedIndexes.clear();
      }
      return std::make_pair(false, !usedIndexes.empty());
    }

    canUseForFilter &= canUseIndex.first;
    canUseForSort |= canUseIndex.second;

    root->changeMember(i, specializedCondition);
  }

  if (canUseForFilter) {
    isSorted = sortOrs(ast, root, reference, usedIndexes);
  }

  // should always be true here. maybe not in the future in case a collection
  // has absolutely no indexes
  return std::make_pair(canUseForFilter, canUseForSort);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief Checks if the index supports the filter condition.
/// note: the caller must have read-locked the underlying collection when
/// calling this method
//////////////////////////////////////////////////////////////////////////////

bool Transaction::supportsFilterCondition(
    IndexHandle const& indexHandle,
    arangodb::aql::AstNode const* condition,
    arangodb::aql::Variable const* reference, size_t itemsInIndex,
    size_t& estimatedItems, double& estimatedCost) {
 
  auto idx = indexHandle.getIndex(); 
  if (nullptr == idx) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "The index id cannot be empty.");
  }

  return idx->supportsFilterCondition(
      condition, reference, itemsInIndex, estimatedItems, estimatedCost);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief Get the index features:
///        Returns the covered attributes, and sets the first bool value
///        to isSorted and the second bool value to isSparse
//////////////////////////////////////////////////////////////////////////////

std::vector<std::vector<arangodb::basics::AttributeName>>
Transaction::getIndexFeatures(IndexHandle const& indexHandle, bool& isSorted,
                              bool& isSparse) {
  
  auto idx = indexHandle.getIndex(); 
  if (nullptr == idx) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "The index id cannot be empty.");
  }

  isSorted = idx->isSorted();
  isSparse = idx->sparse();
  return idx->fields();
}

//////////////////////////////////////////////////////////////////////////////
/// @brief Gets the best fitting index for an AQL sort condition
/// note: the caller must have read-locked the underlying collection when
/// calling this method
//////////////////////////////////////////////////////////////////////////////

std::pair<bool, bool> Transaction::getIndexForSortCondition(
    std::string const& collectionName,
    arangodb::aql::SortCondition const* sortCondition,
    arangodb::aql::Variable const* reference, size_t itemsInIndex,
    std::vector<IndexHandle>& usedIndexes,
    size_t& coveredAttributes) {
  // We do not have a condition. But we have a sort!
  if (!sortCondition->isEmpty() && sortCondition->isOnlyAttributeAccess() &&
      sortCondition->isUnidirectional()) {
    double bestCost = 0.0;
    std::shared_ptr<Index> bestIndex;

    auto indexes = indexesForCollection(collectionName);

    for (auto const& idx : indexes) {
      if (idx->sparse()) {
        // a sparse index may exclude some documents, so it can't be used to
        // get a sorted view of the ENTIRE collection
        continue;
      }
      double sortCost = 0.0;
      size_t covered = 0;
      if (indexSupportsSort(idx.get(), reference, sortCondition, itemsInIndex,
                            sortCost, covered)) {
        if (bestIndex == nullptr || sortCost < bestCost) {
          bestCost = sortCost;
          bestIndex = idx;
          coveredAttributes = covered;
        }
      }
    }

    if (bestIndex != nullptr) {
      usedIndexes.emplace_back(bestIndex);
    }

    return std::make_pair(false, bestIndex != nullptr);
  }

  // No Index and no sort condition that
  // can be supported by an index.
  // Nothing to do here.
  return std::make_pair(false, false);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief factory for OperationCursor objects from AQL
/// note: the caller must have read-locked the underlying collection when
/// calling this method
//////////////////////////////////////////////////////////////////////////////

std::shared_ptr<OperationCursor> Transaction::indexScanForCondition(
    std::string const& collectionName, IndexHandle const& indexId,
    arangodb::aql::Ast* ast, arangodb::aql::AstNode const* condition,
    arangodb::aql::Variable const* var, uint64_t limit, uint64_t batchSize,
    bool reverse) {

  if (ServerState::instance()->isCoordinator()) {
    // The index scan is only available on DBServers and Single Server.
    THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_ONLY_ON_DBSERVER);
  }

  if (limit == 0) {
    // nothing to do
    return std::make_shared<OperationCursor>(TRI_ERROR_NO_ERROR);
  }

  // Now collect the Iterator
  IndexIteratorContext ctxt(_vocbase, resolver());
 
  auto idx = indexId.getIndex();
  if (nullptr == idx) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "The index id cannot be empty.");
  }

  std::unique_ptr<IndexIterator> iterator(idx->iteratorForCondition(this, &ctxt, ast, condition, var, reverse));

  if (iterator == nullptr) {
    // We could not create an ITERATOR and it did not throw an error itself
    return std::make_shared<OperationCursor>(TRI_ERROR_OUT_OF_MEMORY);
  }

  return std::make_shared<OperationCursor>(
      transactionContext()->orderCustomTypeHandler(), iterator.release(), limit,
      batchSize);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief factory for OperationCursor objects
/// note: the caller must have read-locked the underlying collection when
/// calling this method
//////////////////////////////////////////////////////////////////////////////

std::shared_ptr<OperationCursor> Transaction::indexScan(
    std::string const& collectionName, CursorType cursorType,
    IndexHandle const& indexId, VPackSlice const search, uint64_t skip,
    uint64_t limit, uint64_t batchSize, bool reverse) {
  // For now we assume indexId is the iid part of the index.

  if (ServerState::instance()->isCoordinator()) {
    // The index scan is only available on DBServers and Single Server.
    THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_ONLY_ON_DBSERVER);
  }

  if (limit == 0) {
    // nothing to do
    return std::make_shared<OperationCursor>(TRI_ERROR_NO_ERROR);
  }

  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName); 
  TRI_document_collection_t* document = documentCollection(trxCollection(cid));

  std::unique_ptr<IndexIterator> iterator;

  switch (cursorType) {
    case CursorType::ANY: {
      // We do not need search values
      TRI_ASSERT(search.isNone());
      // We do not need an index either
      TRI_ASSERT(nullptr == indexId.getIndex());

      arangodb::PrimaryIndex* idx = document->primaryIndex();

      if (idx == nullptr) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_ARANGO_INDEX_NOT_FOUND,
            "Could not find primary index in collection '" + collectionName + "'.");
      }

      iterator.reset(idx->anyIterator(this));
      break;
    }
    case CursorType::ALL: {
      // We do not need search values
      TRI_ASSERT(search.isNone());
      // We do not need an index either
      TRI_ASSERT(nullptr == indexId.getIndex());

      arangodb::PrimaryIndex* idx = document->primaryIndex();

      if (idx == nullptr) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_ARANGO_INDEX_NOT_FOUND,
            "Could not find primary index in collection '" + collectionName + "'.");
      }

      iterator.reset(idx->allIterator(this, reverse));
      break;
    }
    case CursorType::INDEX: {
      auto idx = indexId.getIndex();
      if (nullptr == idx) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                       "The index id cannot be empty.");
      }
      // Normalize the search values
      // VPackBuilder expander;
      // idx->expandInSearchValues(search, expander);

      // Now collect the Iterator
      IndexIteratorContext ctxt(_vocbase, resolver());
      // iterator.reset(idx->iteratorForSlice(this, &ctxt, expander.slice(), reverse));
      iterator.reset(idx->iteratorForSlice(this, &ctxt, search, reverse));
    }
  }
  if (iterator == nullptr) {
    // We could not create an ITERATOR and it did not throw an error itself
    return std::make_shared<OperationCursor>(TRI_ERROR_OUT_OF_MEMORY);
  }

  uint64_t unused = 0;
  iterator->skip(skip, unused);

  return std::make_shared<OperationCursor>(
      transactionContext()->orderCustomTypeHandler(), iterator.release(), limit,
      batchSize);
}
  
////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection
////////////////////////////////////////////////////////////////////////////////

TRI_document_collection_t* Transaction::documentCollection(
      TRI_transaction_collection_t const* trxCollection) const {
  TRI_ASSERT(_trx != nullptr);
  TRI_ASSERT(trxCollection != nullptr);
  TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);
  TRI_ASSERT(trxCollection->_collection != nullptr);
  TRI_ASSERT(trxCollection->_collection->_collection != nullptr);

  return trxCollection->_collection->_collection;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection
////////////////////////////////////////////////////////////////////////////////

TRI_document_collection_t* Transaction::documentCollection(
      TRI_voc_cid_t cid) const {
  TRI_ASSERT(_trx != nullptr);
  TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);
  
  auto trxCollection = TRI_GetCollectionTransaction(_trx, cid, TRI_TRANSACTION_READ);
  TRI_ASSERT(trxCollection->_collection != nullptr);
  TRI_ASSERT(trxCollection->_collection->_collection != nullptr);

  return trxCollection->_collection->_collection;
}
  
////////////////////////////////////////////////////////////////////////////////
/// @brief add a collection by id, with the name supplied
////////////////////////////////////////////////////////////////////////////////

int Transaction::addCollection(TRI_voc_cid_t cid, char const* name,
                    TRI_transaction_type_e type) {
  int res = this->addCollection(cid, type);

  if (res != TRI_ERROR_NO_ERROR) {
    _errorData = std::string(name);
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief add a collection by id, with the name supplied
////////////////////////////////////////////////////////////////////////////////

int Transaction::addCollection(TRI_voc_cid_t cid, std::string const& name,
                    TRI_transaction_type_e type) {
  return addCollection(cid, name.c_str(), type);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief add a collection by id
////////////////////////////////////////////////////////////////////////////////

int Transaction::addCollection(TRI_voc_cid_t cid, TRI_transaction_type_e type) {
  if (_trx == nullptr) {
    return registerError(TRI_ERROR_INTERNAL);
  }

  if (cid == 0) {
    // invalid cid
    return registerError(TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND);
  }

  if (_setupState != TRI_ERROR_NO_ERROR) {
    return _setupState;
  }

  TRI_transaction_status_e const status = getStatus();

  if (status == TRI_TRANSACTION_COMMITTED ||
      status == TRI_TRANSACTION_ABORTED) {
    // transaction already finished?
    return registerError(TRI_ERROR_TRANSACTION_INTERNAL);
  }

  if (this->isEmbeddedTransaction()) {
   return this->addCollectionEmbedded(cid, type);
  } 

  return this->addCollectionToplevel(cid, type);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief add a collection by name
////////////////////////////////////////////////////////////////////////////////

int Transaction::addCollection(std::string const& name, TRI_transaction_type_e type) {
  if (_setupState != TRI_ERROR_NO_ERROR) {
    return _setupState;
  }

  return addCollection(this->resolver()->getCollectionId(name),
                       name.c_str(), type);
}
  
////////////////////////////////////////////////////////////////////////////////
/// @brief test if a collection is already locked
////////////////////////////////////////////////////////////////////////////////

bool Transaction::isLocked(TRI_document_collection_t* document,
                TRI_transaction_type_e type) {
  if (_trx == nullptr || getStatus() != TRI_TRANSACTION_RUNNING) {
    return false;
  }

  TRI_transaction_collection_t* trxCollection =
      TRI_GetCollectionTransaction(_trx, document->_info.id(), type);
  TRI_ASSERT(trxCollection != nullptr);
  return TRI_IsLockedCollectionTransaction(trxCollection, type, _nestingLevel);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief read- or write-lock a collection
////////////////////////////////////////////////////////////////////////////////

int Transaction::lock(TRI_transaction_collection_t* trxCollection,
           TRI_transaction_type_e type) {
  if (_trx == nullptr || getStatus() != TRI_TRANSACTION_RUNNING) {
    return TRI_ERROR_TRANSACTION_INTERNAL;
  }

  return TRI_LockCollectionTransaction(trxCollection, type, _nestingLevel);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief read- or write-unlock a collection
////////////////////////////////////////////////////////////////////////////////

int Transaction::unlock(TRI_transaction_collection_t* trxCollection,
             TRI_transaction_type_e type) {
  if (_trx == nullptr || getStatus() != TRI_TRANSACTION_RUNNING) {
    return TRI_ERROR_TRANSACTION_INTERNAL;
  }

  return TRI_UnlockCollectionTransaction(trxCollection, type, _nestingLevel);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get list of indexes for a collection
////////////////////////////////////////////////////////////////////////////////

std::vector<std::shared_ptr<Index>> Transaction::indexesForCollection(
    std::string const& collectionName) {

  auto ss = ServerState::instance();
  if (ss->isCoordinator()) {
    return indexesForCollectionCoordinator(collectionName);
  }
  // For a DBserver we use the local case.

  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName); 
  TRI_document_collection_t* document = documentCollection(trxCollection(cid));

  // Wrap fake shared pointers around the raw pointers:
  std::shared_ptr<Index> dummy;
  auto const& intermediate = document->allIndexes();
  std::vector<std::shared_ptr<Index>> result;
  result.reserve(intermediate.size());
  for (auto* p : intermediate) {
    result.emplace_back(dummy, p);
  }
  return result;
}

//////////////////////////////////////////////////////////////////////////////
/// @brief Get all indexes for a collection name, coordinator case
//////////////////////////////////////////////////////////////////////////////

std::shared_ptr<Index> Transaction::indexForCollectionCoordinator(
    std::string const& name, std::string const& id) const {
  auto clusterInfo = arangodb::ClusterInfo::instance();
  auto collectionInfo =
      clusterInfo->getCollection(std::string(_vocbase->_name), name);

  if (collectionInfo.get() == nullptr || (*collectionInfo).empty()) {
    THROW_ARANGO_EXCEPTION_FORMAT(TRI_ERROR_INTERNAL,
                                  "collection not found '%s' in database '%s'",
                                  name.c_str(), _vocbase->_name);
  }

  TRI_json_t const* json = (*collectionInfo).getIndexes();
  auto indexBuilder = arangodb::basics::JsonHelper::toVelocyPack(json);
  VPackSlice const slice = indexBuilder->slice();

  if (slice.isArray()) {
    for (auto const& v : VPackArrayIterator(slice)) {
      if (!v.isObject()) {
        continue;
      }
      VPackSlice const idSlice = v.get("id");
      if (!idSlice.isString()) {
        // No id attribute. It is invalid.
        continue;
      }
      std::string idxId = idSlice.copyString();
      if (idxId == id) {
        // We found the index we looked for
        VPackSlice const type = v.get("type");
        if (!type.isString()) {
          // no "type" attribute. this is invalid.
          break;
        }
        std::string typeString = type.copyString();
        arangodb::Index::IndexType indexType 
            = arangodb::Index::type(typeString.c_str());
        std::shared_ptr<arangodb::Index> idx;
        if (indexType == arangodb::Index::TRI_IDX_TYPE_PRIMARY_INDEX) {
          idx.reset(new arangodb::PrimaryIndex(v));
        } else if (indexType  == arangodb::Index::TRI_IDX_TYPE_EDGE_INDEX) {
          idx.reset(new arangodb::EdgeIndex(v));
        } else if (indexType  == arangodb::Index::TRI_IDX_TYPE_HASH_INDEX) {
          idx.reset(new arangodb::HashIndex(v));
        } else if (indexType  == arangodb::Index::TRI_IDX_TYPE_SKIPLIST_INDEX) {
          idx.reset(new arangodb::SkiplistIndex(v));
        } else if (indexType  == arangodb::Index::TRI_IDX_TYPE_ROCKSDB_INDEX) {
          idx.reset(new arangodb::RocksDBIndex(v));
        }
        return idx;
      }
    }
  }
  return nullptr;
}

//////////////////////////////////////////////////////////////////////////////
/// @brief Get all indexes for a collection name, coordinator case
//////////////////////////////////////////////////////////////////////////////

std::vector<std::shared_ptr<Index>> Transaction::indexesForCollectionCoordinator(std::string const& name) const {

  std::vector<std::shared_ptr<Index>> indexes;

  auto clusterInfo = arangodb::ClusterInfo::instance();
  auto collectionInfo =
      clusterInfo->getCollection(std::string(_vocbase->_name), name);

  if (collectionInfo.get() == nullptr || (*collectionInfo).empty()) {
    THROW_ARANGO_EXCEPTION_FORMAT(TRI_ERROR_INTERNAL,
                                  "collection not found '%s' in database '%s'",
                                  name.c_str(), _vocbase->_name);
  }

  TRI_json_t const* json = collectionInfo->getIndexes();
  auto indexBuilder = arangodb::basics::JsonHelper::toVelocyPack(json);
  VPackSlice const slice = indexBuilder->slice();

  if (slice.isArray()) {
    size_t const n = static_cast<size_t>(slice.length());
    indexes.reserve(n);

    for (auto const& v : VPackArrayIterator(slice)) {
      if (!v.isObject()) {
        continue;
      }
      VPackSlice const type = v.get("type");

      if (!type.isString()) {
        // no "type" attribute. this is invalid
        continue;
      }
      std::string typeString = type.copyString();
      arangodb::Index::IndexType indexType 
          = arangodb::Index::type(typeString.c_str());

      std::shared_ptr<arangodb::Index> idx;
      if (indexType == arangodb::Index::TRI_IDX_TYPE_PRIMARY_INDEX) {
        idx.reset(new arangodb::PrimaryIndex(v));
      } else if (indexType  == arangodb::Index::TRI_IDX_TYPE_EDGE_INDEX) {
        idx.reset(new arangodb::EdgeIndex(v));
      } else if (indexType  == arangodb::Index::TRI_IDX_TYPE_HASH_INDEX) {
        idx.reset(new arangodb::HashIndex(v));
      } else if (indexType  == arangodb::Index::TRI_IDX_TYPE_SKIPLIST_INDEX) {
        idx.reset(new arangodb::SkiplistIndex(v));
      }
      if (idx != nullptr) {
        indexes.push_back(idx);
      }
    }
  }
  return indexes;
}

//////////////////////////////////////////////////////////////////////////////
/// @brief get the index by it's identifier. Will either throw or
///        return a valid index. nullptr is impossible.
//////////////////////////////////////////////////////////////////////////////

Transaction::IndexHandle Transaction::getIndexByIdentifier(
    std::string const& collectionName, std::string const& indexHandle) {

  if (ServerState::instance()->isCoordinator()) {
    if (indexHandle.empty()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                     "The index id cannot be empty.");
    }

    if (!arangodb::Index::validateId(indexHandle.c_str())) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_INDEX_HANDLE_BAD);
    }

    std::shared_ptr<Index> idx =
        indexForCollectionCoordinator(collectionName, indexHandle);

    if (idx == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_INDEX_NOT_FOUND,
                                     "Could not find index '" + indexHandle +
                                         "' in collection '" +
                                         collectionName + "'.");
    }

    // We have successfully found an index with the requested id.
    return IndexHandle(idx);
  }

  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName); 
  TRI_document_collection_t* document = documentCollection(trxCollection(cid));

  if (indexHandle.empty()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "The index id cannot be empty.");
  }

  if (!arangodb::Index::validateId(indexHandle.c_str())) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_INDEX_HANDLE_BAD);
  }
  TRI_idx_iid_t iid = arangodb::basics::StringUtils::uint64(indexHandle);
  arangodb::Index* idx = document->lookupIndex(iid);

  if (idx == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_INDEX_NOT_FOUND,
                                   "Could not find index '" + indexHandle +
                                       "' in collection '" +
                                       collectionName + "'.");
  }
  
  // We have successfully found an index with the requested id.
  std::shared_ptr<arangodb::Index> dummy;
  return IndexHandle(std::shared_ptr<arangodb::Index>(dummy, idx));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief add a collection to an embedded transaction
////////////////////////////////////////////////////////////////////////////////

int Transaction::addCollectionEmbedded(TRI_voc_cid_t cid, TRI_transaction_type_e type) {
  TRI_ASSERT(_trx != nullptr);

  int res = TRI_AddCollectionTransaction(_trx, cid, type, _nestingLevel,
                                         false, _allowImplicitCollections);

  if (res != TRI_ERROR_NO_ERROR) {
    return registerError(res);
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief add a collection to a top-level transaction
////////////////////////////////////////////////////////////////////////////////

int Transaction::addCollectionToplevel(TRI_voc_cid_t cid, TRI_transaction_type_e type) {
  TRI_ASSERT(_trx != nullptr);

  int res;

  if (getStatus() != TRI_TRANSACTION_CREATED) {
    // transaction already started?
    res = TRI_ERROR_TRANSACTION_INTERNAL;
  } else {
    res = TRI_AddCollectionTransaction(_trx, cid, type, _nestingLevel, false,
                                       _allowImplicitCollections);
  }

  if (res != TRI_ERROR_NO_ERROR) {
    registerError(res);
  }

  return res;
}
  
////////////////////////////////////////////////////////////////////////////////
/// @brief initialize the transaction
/// this will first check if the transaction is embedded in a parent
/// transaction. if not, it will create a transaction of its own
////////////////////////////////////////////////////////////////////////////////

int Transaction::setupTransaction() {
  // check in the context if we are running embedded
  _trx = this->_transactionContext->getParentTransaction();

  if (_trx != nullptr) {
    // yes, we are embedded
    _setupState = setupEmbedded();
  } else {
    // non-embedded
    _setupState = setupToplevel();
  }

  // this may well be TRI_ERROR_NO_ERROR...
  return _setupState;
}
  
////////////////////////////////////////////////////////////////////////////////
/// @brief set up an embedded transaction
////////////////////////////////////////////////////////////////////////////////

int Transaction::setupEmbedded() {
  TRI_ASSERT(_nestingLevel == 0);

  _nestingLevel = ++_trx->_nestingLevel;

  if (!this->_transactionContext->isEmbeddable()) {
    // we are embedded but this is disallowed...
    return TRI_ERROR_TRANSACTION_NESTED;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief set up a top-level transaction
////////////////////////////////////////////////////////////////////////////////

int Transaction::setupToplevel() {
  TRI_ASSERT(_nestingLevel == 0);

  // we are not embedded. now start our own transaction
  _trx = TRI_CreateTransaction(_vocbase, _externalId, _timeout, _waitForSync);

  if (_trx == nullptr) {
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  // register the transaction in the context
  return this->_transactionContext->registerTransaction(_trx);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief free transaction
////////////////////////////////////////////////////////////////////////////////

void Transaction::freeTransaction() {
  TRI_ASSERT(!isEmbeddedTransaction());

  if (_trx != nullptr) {
    auto id = _trx->_id;
    bool hasFailedOperations = TRI_FreeTransaction(_trx);
    _trx = nullptr;
      
    // store result
    this->_transactionContext->storeTransactionResult(id, hasFailedOperations);
    this->_transactionContext->unregisterTransaction();
  }
}
  
//////////////////////////////////////////////////////////////////////////////
/// @brief constructor, leases a builder
//////////////////////////////////////////////////////////////////////////////

TransactionBuilderLeaser::TransactionBuilderLeaser(arangodb::Transaction* trx) 
      : _transactionContext(trx->transactionContext().get()), 
        _builder(_transactionContext->leaseBuilder()) {
}

//////////////////////////////////////////////////////////////////////////////
/// @brief constructor, leases a builder
//////////////////////////////////////////////////////////////////////////////

TransactionBuilderLeaser::TransactionBuilderLeaser(arangodb::TransactionContext* transactionContext) 
      : _transactionContext(transactionContext), 
        _builder(_transactionContext->leaseBuilder()) {
}

//////////////////////////////////////////////////////////////////////////////
/// @brief destructor, returns a builder
//////////////////////////////////////////////////////////////////////////////

TransactionBuilderLeaser::~TransactionBuilderLeaser() { 
  _transactionContext->returnBuilder(_builder); 
}

