# fts-elastic
fts-elastic is a Dovecot full-text search indexing plugin that uses ElasticSearch as a backend.

Dovecot communicates to ES using HTTP/JSON queries. It supports automatic indexing and searching of e-mail.

## Requirements
* Dovecot 2.2+
* JSON-C
* ElasticSearch 6.x for your server
* Autoconf 2.53+

## Compiling
This plugin needs to compile against the Dovecot source for the version you intend to run it on. A dovecot-devel package is unfortunately insufficient as it does not include the required fts API header files. 

You can provide the path to your source tree by passing --with-dovecot= to ./configure.

An example build may look like:

    ./autogen.sh
    ./configure --with-dovecot=/usr/lib/dovecot/
    make
    make install
	# optionally
	sudo ln -s /usr/lib/dovecot/lib21_fts_elastic_plugin.so /usr/lib/dovecot/modules/lib21_fts_elastic_plugin.so

## Configuration
In dovecot/conf.d/10-mail.conf:

	mail_plugins = fts fts_elastic

In dovecot/conf.d/90-plugins.conf:

	plugin {
	  fts = elastic
	  fts_elastic = debug url=http://localhost:9200/m/ bulk_size=5000000 refresh=fts rawlog_dir=/var/log/fts-elastic/
	  fts_autoindex = yes
	}

There are only two supported configuration parameters at the moment:
* url=\<elasticsearch url\> Required elastic URL with index name, must end with slash /
* bulk_size=\<positive integer\> How large bulk requests we want to send to elastic in bytes (default=5000000)
* refresh={fts,index,never} When to refresh elastic index
  * fts: when parent dovecot fts plugin calls it (typically before search)
  * index: after each bulk update using ?refrest=true query param (create not effective indexes when combined with fts_autoindex=yes)
  * never: leave it to elastic, indexed emails may not be searchable immediately
* debug Enables HTTP debugging
* rawlog_dir is directory where HTTP communication with elasticsearch server is written (useful for debugging plugin or elastic schema)

## ElasticSearch Indicies
fts-elastic index all message in on index. It creates one field for each field in the e-mail header and for the body.
_id is in the form "_id":"uid/mbox-guid/user@domain", example: "_id":"3/f40efa2f8f44ad54424000006e8130ae/filip.hanes@example.com"
Fields box and user needs to be keyword fields.

You can setup index on Elasticsearch with command

	curl -X PUT "http://elasticIP:9200/m?pretty" -H 'Content-Type: application/json' -d "@elastic-schema.json"


An example of pushed data:

	{
		"user": "filip.hanes@example.com",
		"box": "f40efa2f8f44ad54424000006e8130ae",
		"uid": 3,
		"date": "Thu, 08 Jan 2015 00:20:05 +0000",
		"from": "josh <josh@localhost.localdomain>",
		"sender": "Filip Hanes",
		"to": "<filip.hanes@example.com>",
		"cc": "User <user@example.com>",
		"bcc": "\"Test User\" <test@example.com>",
		"subject": "Test #3",
		"message-id": "<20150107132005.07DA3140314@example.com>",
		"body": "This is the body of test #3.\n"
	}

An example search:

```bash
curl -X POST "http://elasticIP:9200/m/_search?pretty" -H 'Content-Type: application/json' -d '
{
  "query": {
    "bool": {
      "must": [
        {"term": {"user": "user@example"}},
        {"term": {"box": "f40efa2f8f44ad54424000006e8130ae"}},
        {
          "multi_match": {
            "query": "test",
            "operator": "and",
            "fields": ["from","to","cc","bcc","sender","subject","body"]
          }
        }
      ]
    }
  },
  "size": 100
}
'
```

## TODO
* user/mbox_gui parametrized url i.e.: url=http://127.0.0.1/m-%u/ would use index http://127.0.0.1/m-filip.hanes@example.com/
* Rescan
* Optimisation (if any)
* Multiple mailbox lookup (for clients that call lookup_multi; need to find one)

## Thanks
This plugin borrows heavily from dovecot itself particularly for the automatic detection of dovecont-config (see m4/dovecot.m4). The fts-solr and fts-squat plugins were also used as reference material for understanding the Dovecot FTS API.
