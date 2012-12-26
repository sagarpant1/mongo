// query.js

if ( typeof DBQuery == "undefined" ){
    DBQuery = function( mongo , db , collection , ns , query , fields , limit , skip , batchSize , options ){
        
        this._mongo = mongo; // 0
        this._db = db; // 1
        this._collection = collection; // 2
        this._ns = ns; // 3
        
        this._query = query || {}; // 4
        this._fields = fields; // 5
        this._limit = limit || 0; // 6
        this._skip = skip || 0; // 7
        this._batchSize = batchSize || 0;
        this._options = options || 0;

        this._cursor = null;
        this._numReturned = 0;
        this._special = false;
        this._prettyShell = false;
    }
    print( "DBQuery probably won't have array access " );
}

DBQuery.prototype.help = function () {
    print("find() modifiers")
    print("\t.sort( {...} )")
    print("\t.limit( n )")
    print("\t.skip( n )")
    print("\t.count() - total # of objects matching query, ignores skip,limit")
    print("\t.size() - total # of objects cursor would return, honors skip,limit")
    print("\t.explain([verbose])")
    print("\t.hint(...)")
    print("\t.addOption(n) - adds op_query options -- see wire protocol")
    print("\t._addSpecial(name, value) - http://dochub.mongodb.org/core/advancedqueries#AdvancedQueries-Metaqueryoperators")
    print("\t.batchSize(n) - sets the number of docs to return per getMore")
    print("\t.showDiskLoc() - adds a $diskLoc field to each returned object")
    print("\t.min(idxDoc)")
    print("\t.max(idxDoc)")
    
    print("\nCursor methods");
    print("\t.toArray() - iterates through docs and returns an array of the results")
    print("\t.forEach( func )")
    print("\t.map( func )")
    print("\t.hasNext()")
    print("\t.next()")
    print("\t.objsLeftInBatch() - returns count of docs left in current batch (when exhausted, a new getMore will be issued)")
    print("\t.count(applySkipLimit) - runs command at server")    
    print("\t.itcount() - iterates through documents and counts them")
}

DBQuery.prototype.clone = function(){
    var q =  new DBQuery( this._mongo , this._db , this._collection , this._ns , 
        this._query , this._fields , 
        this._limit , this._skip , this._batchSize , this._options );
    q._special = this._special;
    return q;
}

DBQuery.prototype._ensureSpecial = function() {
    if (this._special)
        return this;

    var query = this._query;

    // Set special flag if the query doc starts with $query/query --
    // This is common when copied from the output of the log or query profiler
    this._special = query && (query["$query"] || query["query"]) ? true : false;
    if (this._special && query) {
        // copy the input query fields to the new (wrapped query) -- converts to correct names
        var wrappedQuery = {};
        for (f in query)
            if (f.endsWith("query")) { // convert [$]query into query
                wrappedQuery["$query"] = query[f];
            } else if (f.endsWith("orderby")) { // convert [$]orderby into
                                                // orderby
                wrappedQuery["orderby"] = query[f];
            } else {
                wrappedQuery[f] = query[f];
            }
        this._query = wrappedQuery;
    } else {
        // just wrap the query
        this._query = { query : this._query };
        this._special = true;
    }
    return this;
}

DBQuery.prototype._checkModify = function(){
    if ( this._cursor )
        throw "query already executed";
}

DBQuery.prototype._exec = function(){
    if ( ! this._cursor ){
        assert.eq( 0 , this._numReturned );
        this._cursor = this._mongo.find( this._ns , this._query , this._fields , this._limit , this._skip , this._batchSize , this._options );
        this._cursorSeen = 0;
    }
    return this._cursor;
}

DBQuery.prototype.limit = function( limit ){
    this._checkModify();
    this._limit = limit;
    return this;
}

DBQuery.prototype.batchSize = function( batchSize ){
    this._checkModify();
    this._batchSize = batchSize;
    return this;
}


DBQuery.prototype.addOption = function( option ){
    this._options |= option;
    return this;
}

DBQuery.prototype.skip = function( skip ){
    this._checkModify();
    this._skip = skip;
    return this;
}

DBQuery.prototype.hasNext = function(){
    this._exec();

    if ( this._limit > 0 && this._cursorSeen >= this._limit )
        return false;
    var o = this._cursor.hasNext();
    return o;
}

DBQuery.prototype.next = function(){
    this._exec();
    
    var o = this._cursor.hasNext();
    if ( o )
        this._cursorSeen++;
    else
        throw "error hasNext: " + o;
    
    var ret = this._cursor.next();
    if ( ret.$err && this._numReturned == 0 && ! this.hasNext() )
        throw "error: " + tojson( ret );

    this._numReturned++;
    return ret;
}

DBQuery.prototype.objsLeftInBatch = function(){
    this._exec();

    var ret = this._cursor.objsLeftInBatch();
    if ( ret.$err )
        throw "error: " + tojson( ret );

    return ret;
}

DBQuery.prototype.readOnly = function(){
    this._exec();
    this._cursor.readOnly();
    return this;
}

DBQuery.prototype.toArray = function(){
    if ( this._arr )
        return this._arr;
    
    var a = [];
    while ( this.hasNext() )
        a.push( this.next() );
    this._arr = a;
    return a;
}

DBQuery.prototype.count = function( applySkipLimit ){
    var cmd = { count: this._collection.getName() };
    if ( this._query ){
        if ( this._special )
            cmd.query = this._query.query;
        else 
            cmd.query = this._query;
    }
    cmd.fields = this._fields || {};

    if ( applySkipLimit ){
        if ( this._limit )
            cmd.limit = this._limit;
        if ( this._skip )
            cmd.skip = this._skip;
    }
    
    var res = this._db.runCommand( cmd );
    if( res && res.n != null ) return res.n;
    throw "count failed: " + tojson( res );
}

DBQuery.prototype.size = function(){
    return this.count( true );
}

DBQuery.prototype.countReturn = function(){
    var c = this.count();

    if ( this._skip )
        c = c - this._skip;

    if ( this._limit > 0 && this._limit < c )
        return this._limit;
    
    return c;
}

/**
* iterative count - only for testing
*/
DBQuery.prototype.itcount = function(){
    var num = 0;
    while ( this.hasNext() ){
        num++;
        this.next();
    }
    return num;
}

DBQuery.prototype.length = function(){
    return this.toArray().length;
}

DBQuery.prototype._addSpecial = function( name , value ){
    this._ensureSpecial();
    this._query[name] = value;
    return this;
}

DBQuery.prototype.sort = function( sortBy ){
    return this._addSpecial( "orderby" , sortBy );
}

DBQuery.prototype.hint = function( hint ){
    return this._addSpecial( "$hint" , hint );
}

DBQuery.prototype.min = function( min ) {
    return this._addSpecial( "$min" , min );
}

DBQuery.prototype.max = function( max ) {
    return this._addSpecial( "$max" , max );
}

DBQuery.prototype.showDiskLoc = function() {
    return this._addSpecial( "$showDiskLoc" , true);
}

/**
 * Sets the read preference for this cursor.
 * 
 * @param mode {string} read prefrence mode to use.
 * @param tagSet {Array.<Object>} optional. The list of tags to use, order matters.
 *     Note that this object only keeps a shallow copy of this array.
 * 
 * @return this cursor
 */
DBQuery.prototype.readPref = function( mode, tagSet ) {
    var readPrefObj = {
        mode: mode
    };

    if ( tagSet ){
        readPrefObj.tags = tagSet;
    }

    return this._addSpecial( "$readPreference", readPrefObj );
};

DBQuery.prototype.forEach = function( func ){
    while ( this.hasNext() )
        func( this.next() );
}

DBQuery.prototype.map = function( func ){
    var a = [];
    while ( this.hasNext() )
        a.push( func( this.next() ) );
    return a;
}

DBQuery.prototype.arrayAccess = function( idx ){
    return this.toArray()[idx];
}
DBQuery.prototype.comment = function (comment) {
    var n = this.clone();
    n._ensureSpecial();
    n._addSpecial("$comment", comment);
    return this.next();
}

DBQuery.prototype.explain = function (verbose) {
    /* verbose=true --> include allPlans, oldPlan fields */
    var n = this.clone();
    n._ensureSpecial();
    n._query.$explain = true;
    n._limit = Math.abs(n._limit) * -1;
    var e = n.next();

    function cleanup(obj){
        if (typeof(obj) != 'object'){
            return;
        }

        delete obj.allPlans;
        delete obj.oldPlan;

        if (typeof(obj.length) == 'number'){
            for (var i=0; i < obj.length; i++){
                cleanup(obj[i]);
            }
        }

        if (obj.shards){
            for (var key in obj.shards){
                cleanup(obj.shards[key]);
            }
        }

        if (obj.clauses){
            cleanup(obj.clauses);
        }
    }

    if (!verbose)
        cleanup(e);

    return e;
}

DBQuery.prototype.snapshot = function(){
    this._addSpecial("$snapshot", true);
    return this;
}

DBQuery.prototype.pretty = function(){
    this._prettyShell = true;
    return this;
}

DBQuery.prototype.shellPrint = function(){
    try {
        var start = new Date().getTime();
        var n = 0;
        while ( this.hasNext() && n < DBQuery.shellBatchSize ){
            var s = this._prettyShell ? tojson( this.next() ) : tojson( this.next() , "" , true );
            print( s );
            n++;
        }
        if (typeof _verboseShell !== 'undefined' && _verboseShell) {
            var time = new Date().getTime() - start;
            print("Fetched " + n + " record(s) in " + time + "ms");
        }
         if ( this.hasNext() ){
            print( "Type \"it\" for more" );
            ___it___  = this;
        }
        else {
            ___it___  = null;
        }
   }
    catch ( e ){
        print( e );
    }
    
}

DBQuery.prototype.toString = function(){
    return "DBQuery: " + this._ns + " -> " + tojson( this._query );
}

DBQuery.shellBatchSize = 20;

/**
 * Query option flag bit constants.
 * @see http://dochub.mongodb.org/core/mongowireprotocol#MongoWireProtocol-OPQUERY
 */
DBQuery.Option = {
    tailable: 0x2,
    slaveOk: 0x4,
    oplogReplay: 0x8,
    noTimeout: 0x10,
    awaitData: 0x20,
    exhaust: 0x40,
    partial: 0x80
};

function DBCommandCursor(mongo, cmdResult, batchSize) {
    assert.commandWorked(cmdResult)
    this._firstBatch = cmdResult.cursor.firstBatch.reverse(); // modifies input to allow popping
    this._cursor = mongo.cursorFromId(cmdResult.cursor.ns, cmdResult.cursor.id, batchSize);
}

DBCommandCursor.prototype = {};
DBCommandCursor.prototype.hasNext = function() {
    return this._firstBatch.length || this._cursor.hasNext();
}
DBCommandCursor.prototype.next = function() {
    if (this._firstBatch.length) {
        // $err wouldn't be in _firstBatch since ok was true.
        return this._firstBatch.pop();
    }
    else {
        var ret = this._cursor.next();
        if ( ret.$err )
            throw "error: " + tojson( ret );
        return ret;
    }
}
DBCommandCursor.prototype.objsLeftInBatch = function() {
    if (this._firstBatch.length) {
        return this._firstBatch.length;
    }
    else {
        return this._cursor.objsLeftInBatch();
    }
}

// Copy these methods from DBQuery
DBCommandCursor.prototype.toArray = DBQuery.prototype.toArray
DBCommandCursor.prototype.forEach = DBQuery.prototype.forEach
DBCommandCursor.prototype.map = DBQuery.prototype.map
DBCommandCursor.prototype.itcount = DBQuery.prototype.itcount
DBCommandCursor.prototype.shellPrint = DBQuery.prototype.shellPrint
DBCommandCursor.prototype.pretty = DBQuery.prototype.pretty

