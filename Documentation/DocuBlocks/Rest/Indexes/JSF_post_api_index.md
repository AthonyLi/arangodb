////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_post_api_index
/// @brief creates an index
///
/// @RESTHEADER{POST /_api/index#general, Create index}
///
/// @RESTQUERYPARAMETERS
///
/// @RESTQUERYPARAM{collection,string,required}
/// The collection name.
///
/// @RESTALLBODYPARAM{index-details,json,required}
///
/// @RESTDESCRIPTION
/// **NOTE** Swagger examples won't work due to the anchor.
///
///
/// Creates a new index in the collection *collection*. Expects
/// an object containing the index details.
///
/// The type of the index to be created must specified in the *type*
/// attribute of the index details. Depending on the index type, additional
/// other attributes may need to specified in the request in order to create
/// the index.
///
/// Most indexes (a notable exception being the cap constraint) require the
/// array of attributes to be indexed in the *fields* attribute of the index
/// details. Depending on the index type, a single attribute or multiple
/// attributes can be indexed.
///
/// Indexing system attributes such as *_id*, *_key*, *_from*, and *_to*
/// is not supported for user-defined indexes. Manually creating an index using
/// any of these attributes will fail with an error.
///
/// Some indexes can be created as unique or non-unique variants. Uniqueness
/// can be controlled for most indexes by specifying the *unique* flag in the
/// index details. Setting it to *true* will create a unique index.
/// Setting it to *false* or omitting the *unique* attribute will
/// create a non-unique index.
///
/// **Note**: The following index types do not support uniqueness, and using
/// the *unique* attribute with these types may lead to an error:
///
/// - cap constraints
/// - fulltext indexes
///
/// **Note**: Unique indexes on non-shard keys are not supported in a
/// cluster.
///
/// Hash and skiplist indexes can optionally be created in a sparse
/// variant. A sparse index will be created if the *sparse* attribute in
/// the index details is set to *true*. Sparse indexes do not index documents
/// for which any of the index attributes is either not set or is *null*. 
///
/// @RESTRETURNCODES
///
/// @RESTRETURNCODE{200}
/// If the index already exists, then an *HTTP 200* is returned.
///
/// @RESTRETURNCODE{201}
/// If the index does not already exist and could be created, then an *HTTP 201*
/// is returned.
///
/// @RESTRETURNCODE{400}
/// If an invalid index description is posted or attributes are used that the
/// target index will not support, then an *HTTP 400* is returned.
///
/// @RESTRETURNCODE{404}
/// If *collection* is unknown, then an *HTTP 404* is returned.
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////