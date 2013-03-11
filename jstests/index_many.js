/* test using lots of indexes on one collection */

t = db.jstests_index_many;

function f() {

    t.drop();
    db.many2.drop();

    t.save({ x: 9, y : 99 });
    t.save({ x: 19, y : 99 });

    x = 2;
    while (x < 70) {
        patt = {};
        patt[x] = 1;
        if (x == 20)
            patt = { x: 1 };
        if (x == 64)
            patt = { y: 1 };
        t.ensureIndex(patt);
        x++;
    }

    // print( tojson(db.getLastErrorObj()) );
    assert(db.getLastError(), "should have got an error 'too many indexes'");

    // 40 is the limit currently
    lim = t.getIndexes().length;
    if (lim != 64) {
        print("# of indexes should be 64 but is : " + lim);
        return;
    }
    assert(lim == 64, "not 64 indexes");

    assert(t.find({ x: 9 }).length() == 1, "b");
    assert(t.find({ x: 9 }).explain().cursor.match(/Index/), "not using index?");

    assert(t.find({ y: 99 }).length() == 2, "y idx");
    assert(t.find({ y: 99 }).explain().cursor.match(/Index/), "not using y index?");
}

f();
