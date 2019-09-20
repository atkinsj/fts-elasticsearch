# fts-elastic
fts-elastic is a [Dovecot full-text search](https://doc.dovecot.org/configuration_manual/fts/) indexing plugin that uses [ElasticSearch](https://www.elastic.co/) as a backend.

Dovecot communicates to ES using HTTP/JSON queries. It supports automatic indexing and searching of e-mail.

## Requirements
* Dovecot 2.2+
* JSON-C
* ElasticSearch 6.x, 7.x
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

	  fts_autoindex = no
    fts_autoindex_exclude = \Junk
    fts_autoindex_exclude2 = \Trash
	}

* url=\<elasticsearch url\> Required elastic URL with index name, must end with slash /
* bulk_size=\<positive integer\> How large bulk requests we want to send to elastic in bytes (default=5000000)
* refresh={fts,index,never} When you want to [refresh elastic index](https://www.elastic.co/guide/en/elasticsearch/reference/current/indices-refresh.html) so new emails will be searchable
  * fts: when dovecot fts plugin calls it (typically before search)
  * index: after each bulk update using ?refrest=true query param (create not effective indexes when combined with fts_autoindex=yes)
  * never: leave it to elastic, indexed emails may not be searchable immediately
* debug Enables HTTP debugging
* rawlog_dir is directory where HTTP communication with elasticsearch server is written (useful for debugging plugin or elastic schema)

## ElasticSearch index
This plugin stores all message in one elastic index. You can use [sharding](https://www.elastic.co/guide/en/elasticsearch/reference/current/scalability.html) to support large numbers of users. Since it uses [routing key](https://www.elastic.co/guide/en/elasticsearch/reference/current/mapping-routing-field.html), updates and searches are accessing only one shard.
_id is in the form "_id":"uid/mbox-guid/user@domain", example: "_id":"3/f40efa2f8f44ad54424000006e8130ae/filip.hanes@example.com"

You can setup [index mapping](https://www.elastic.co/guide/en/elasticsearch/reference/current/mapping.html) on Elasticsearch with command

	curl -X PUT "http://elasticIP:9200/m?pretty" -H 'Content-Type: application/json' -d "@elastic-schema.json"

Fields box and user needs to be [keyword fields](https://www.elastic.co/guide/en/elasticsearch/reference/current/keyword.html), as you can see in file `elastic-schema.json`.
In our schema there is [_source](https://www.elastic.co/guide/en/elasticsearch/reference/current/mapping-source-field.html) enabled because we don't see much storage savings when _source is disabled and elastic documentation doesn't recommend it either.
This plugin doesn't use _source. It explicitly disables it in response queries, but you can use it for better management and insight to indexed emails or when you want to use elastic for other than dovecot fts (analysis, spammers detection, ...).
In case of [elastic reindexing](https://www.elastic.co/guide/en/elasticsearch/reference/current/docs-reindex.html) _source will be needed.

Any time you can reindex users mailbox with doveadm commands;

```sh
doveadm fts rescan -u user@example.com
doveadm index -u user@domain -q '*'
```

An example of pushed document:
```json
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
```

An example search:

```sh
curl -X POST "http://elasticIP:9200/m/_search?pretty" -H 'Content-Type: application/json' -d '
{
  "query": {
    "bool": {
      "filter": [
        {"term": {"user": "filip.hanes@example.com"}},
        {"term": {"box": "f40efa2f8f44ad54424000006e8130ae"}}
      ],
      "must": [
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
* Rescan
* Multiple mailbox lookup (for clients that call lookup_multi; need to find one)
* user/mbox_guid parametrized url i.e.: url=http://127.0.0.1/m-%u/ would use index http://127.0.0.1/m-filip.hanes@example.com/
* Optimisation (if any)

## Thanks
This plugin borrows heavily from dovecot itself particularly for the automatic detection of dovecont-config (see m4/dovecot.m4). The fts-solr and fts-squat plugins were also used as reference material for understanding the Dovecot FTS API.
FTS-lucene was used as reference for implementing proper rescan.
