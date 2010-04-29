<?php

//define('SEARCHLIGHT_solr_FILTER_MAX', 100000000000);
//define('SEARCHLIGHT_solr_FILTER_MIN', 0);


class SearchlightBackendSolr extends SearchlightBackend {
  /**
   * Override of settingsDefault().
   */
  function settingsDefault() {
    return array(
      'host' => 'localhost',
      'port' => 8983,
      'path' => '/solr',
    );
  }

  /**
   * Override of settingsForm().
   */
  function settingsForm() {
    $form = array();
    $form['host'] = array(
      '#title' => t('Solr host'),
      '#type' => 'textfield',
      '#default_value' => $this->settings['host'],
      '#size' => 60,
    );
    $form['port'] = array(
      '#title' => t('Solr port'),
      '#type' => 'textfield',
      '#default_value' => $this->settings['port'],
      '#size' => 6,
    );
    $form['path'] = array(
      '#title' => t('Solr path'),
      '#type' => 'textfield',
      '#default_value' => $this->settings['path'],
      '#size' => 60,
    );
    return $form;
  }

  /**
   * Override of initClient().
   */
  function initClient() {
    $this->includeSolr();
    $client = new Apache_Solr_Service($this->settings['host'], (int) $this->settings['port'], $this->settings['path']);
    return $client;
  }

  /**
   * Override of executeQuery().
   */
  function executeQuery(&$client, $datasource, $query = '') {
    // Determine if we need to query a delta and main index.
    $index = $datasource['id'];

    $result = $client->Query($query, $datasource['id']);
    if (!empty($result['matches'])) {
      return array('result' => array_keys($result['matches']), 'total' => $result['total'], 'raw' => $result);
    }
    return FALSE;
  }

  /**
   * Override of setFilter().
   */
  function setFilter(&$client, $datasource, $filters) {
    if (!empty($filters)) {
      // Iterate through once to separate out range filters.
      $range_filters = array();
      foreach ($filters as $key => $params) {
        $field    = $params['field'];
        $operator = $params['operator'];
        $args     = $params['args'];
        if (in_array($operator, array('<', '>', '>=', '<='))) {
          unset($filters[$key]);
          switch ($operator) {
            case '>':
            case '>=':
              $range_filters[$field]['field'] = $field;
              $range_filters[$field]['max'] = isset($range_filters[$field]['max']) ? $range_filters[$field]['max'] : SEARCHLIGHT_solr_FILTER_MAX;
              $range_filters[$field]['min'] = eval("return {$args[0]};");
              $range_filters[$field]['exclude'] = $operator === '>' ? TRUE : FALSE;
              break;
            case '<':
            case '<=':
              $range_filters[$field]['field'] = $field;
              $range_filters[$field]['max'] = eval("return {$args[0]};");
              $range_filters[$field]['min'] = isset($range_filters[$field]['min']) ? $range_filters[$field]['min'] : SEARCHLIGHT_solr_FILTER_MIN;
              $range_filters[$field]['exclude'] = $operator === '<' ? TRUE : FALSE;
              break;
            default:
              break;
          }
        }
      }
      // Now set range filters.
      foreach ($range_filters as $filter) {
        // The exclude option on range filters appears to be broken in
        // solr 0.9.9. Update this code to support either operator once the
        // problem is fixed upstream.
        // $client->SetFilterRange($filter['field'], $filter['min'], $filter['max'], $filter['exclude']);
        $client->SetFilterRange($filter['field'], $filter['min'], $filter['max'], FALSE);
      }
      // Equality & set filters filters.
      foreach ($filters as $params) {
        $field    = $params['field'];
        $operator = $params['operator'];
        $args     = $params['args'];
        switch ($operator) {
          case '=':
            $this->_SetFilter($client, $datasource, $field, $args, FALSE);
            break;
          case 'IN':
            $this->_SetFilter($client, $datasource, $field, $args, FALSE);
            break;
          case 'NOT IN':
            $this->_SetFilter($client, $datasource, $field, $args, TRUE);
            break;
        }
      }
    }
  }

  /**
   * Wrapper of $solr->SetFilter().
   * Convert any string values to ordinals before passing through.
   */
  function _SetFilter(&$client, $datasource, $attribute, $values, $exclude = FALSE) {
    $ordinals = array();
    foreach ($values as $arg) {
      $arg = trim($arg);
      if (is_numeric($arg)) {
        $ordinals[] = $arg;
      }
      else {
        $ordinal = $this->getOrdinal($datasource, $attribute, $arg);
        if (is_numeric($ordinal)) {
          $ordinals[] = $ordinal;
        }
      }
    }
    return $client->SetFilter($attribute, $ordinals, $exclude);
  }

  /**
   * Override of setOptions().
   */
  function setOptions(&$client, $datasource, $options) {
    if (isset($options['matchMode'])) {
      $client->SetMatchMode($options['matchMode']);
    }
  }

  /**
   * Override of setSort().
   */
  function setSort(&$client, $datasource, $sorts) {
    if (!empty($sorts)) {
      $solr_sorts = array();
      foreach ($sorts as $sort) {
        if ($sort['field'] === 'searchlight_weight') {
          $solr_sorts[] = "@weight {$sort['order']}";
        }
        else {
          $solr_sorts[] = "{$sort['field']} {$sort['order']}";
        }
      }
      $client->setSortMode(SPH_SORT_EXTENDED, implode(', ', $solr_sorts));
    }
  }

  /**
   * Override of setPager().
   */
  function setPager(&$client, $offset, $limit) {
    $limit = !empty($limit) ? $limit : 1000;
    if (!empty($offset) || !empty($limit)) {
      $client->SetLimits((int) $offset, (int) $limit);
    }
  }

  /**
   * Override of setNodeAccess().
   * For solr API usage of filter ORs, see:
   * - http://www.solrsearch.com/faq.html#filter-or
   * - http://www.solrsearch.com/forum/view.html?id=3440
   */
  function setNodeAccess(&$client, $grants) {
    if (!empty($grants)) {
      $grants_sql = array();
      foreach ($grants as $realm => $gids) {
        foreach ($gids as $gid) {
          $grants_sql[] = "IF(IN(node_access_gid_{$realm}, {$gid}), 1, 0)";
        }
      }
      if (count($grants_sql)) {
        $grants_sql = implode('+', $grants_sql);
        $client->SetSelect("*, ({$grants_sql}) AS solr_node_access");
        $client->SetFilterRange('solr_node_access', 1, 1000);
      }
    }
  }

  /**
   * Override of facetBuild().
   */
  function facetBuild($query, $field) {
    switch ($field['datatype']) {
      case 'timestamp':
        $groupby = array('day' => SPH_GROUPBY_DAY, 'month' => SPH_GROUPBY_MONTH, 'year' => SPH_GROUPBY_YEAR);
        $granularity = !empty($field['granularity']) ? $field['granularity'] : 'month';
        $query->client->SetGroupBy($field['name'], $groupby[$granularity], '@group desc');
        break;
      default:
        $query->client->SetGroupBy($field['name'], SPH_GROUPBY_ATTR, '@count desc');
        $query->client->SetArrayResult($field['usage'] === 'multivalue');
        break;
    }

    // The facet is active ensure it's limited to 1 result entry. For
    // multivalue facets, multiple entries can be returned even if the
    // facet is active.
    if (searchlight_facets()->getFacet($field['name'])) {
      $query->client->SetLimits(0, 1);
    }
    else {
      // @TODO: make configurable:
      // - # of items to show on a per facet basis
      $query->client->SetLimits(0, 5);
    }

    $result = $this->executeQuery($query->client, $query->datasource, $query->search_query);
    if ($result) {
      $items = array();
      foreach ($result['result'] as $id) {
        if (isset($result['raw']['matches'][$id]['attrs'])) {
          $attr = $result['raw']['matches'][$id]['attrs'];
          if (isset($attr['@groupby'], $attr['@count'])) {
            switch ($field['datatype']) {
              case 'text':
                $id = $this->getOrdinal($query->datasource, $field['name'], $attr['@groupby']);
                break;
              case 'timestamp':
                $id = $this->getTimestamp($attr['@groupby'], !empty($field['granularity']) ? $field['granularity'] : 'month');
                break;
              default:
                $id = $attr['@groupby'];
                break;
            }
            $items[$id] = array('id' => $id, 'count' => $attr['@count']);
          }
        }
      }
      return $items;
    }
    return FALSE;
  }

  function drushSearchd() {
    $jar_path = '/usr/local/apache-solr-1.4.0/example'; // TODO move to option.

    $file_path = conf_path() .'/solr';
    $solr_home = drush_locate_root() .'/'. conf_path() . '/solr';
    $log_dir = $solr_home .'/log';
    if (file_check_directory($log_dir, TRUE)) {
      $opts .= '-Dsolr.solr.home='. $solr_home .' ';
      $opts .= '-Djetty.logs='. $log_dir.' ';
      $opts .= '-Djetty.home='. $jar_path.' ';
      $opts .= '-jar '. $jar_path .'/start.jar';
      drush_op('drush_shell_exec', 'java '. $opts);
    }
    return drush_log("An error ocurred while starting the search daemon.", 'error');
  }

  function drushIndex() {
    // build list of this site's indexes
    $indexes = array();
    foreach (variable_get('searchlight_views', array()) as $base_table => $view) {
      $id = searchlight_get_datasource_id($base_table);
      list($view_name, $display_id) = explode(':', $view);

      // Build and execute the view
      $view = views_get_view($view_name);
      // $o = $view->preview($display_id);
      // Push the contents to Solr.
      // $this->initClient();
      // TODO

      // db_query('UPDATE {searchlight_search} SET status = 1 WHERE status = 0');
      // variable_set('searchlight_solr_last', time());
      return drush_log('Indexing complete.', 'success');
      // return drush_log("An error ocurred while indexing.", 'error');

    }
  }

  function drushWriteConf() {
    $file_path = conf_path() .'/solr';
    if (file_check_directory($file_path, TRUE)) {
      // Collect configuration arrays for each datasource.
      $cores = array();
      foreach (array_keys(variable_get('searchlight_views', array())) as $base_table) {
        if ($datasource = searchlight_get_datasource($base_table, TRUE)) {
          $cores[] = $datasource['id'];
          $core_path = $file_path .'/'. $datasource['id'];
          if (!file_check_directory($core_path, TRUE)) {
            return drush_log("/{$core_path} could not be written to.", 'error');
          }
          $core_path .= '/conf';
          if (file_check_directory($core_path, TRUE)) {
            // Generate configuration file from datasources.
            $datasource = $this->solrDatasourceConf($datasource);
            $files = array(
              'schema.xml' => theme('searchlight_solr_schema', $datasource),
              'solrconfig.xml' => theme('searchlight_solr_config'),
            );
            $this->solrWriteFiles($core_path, $files);
          }
          else {
            return drush_log("/{$core_path} could not be written to.", 'error');
          }
        }
        else {
          return drush_log("Invalid data source.", 'error');
        }
      }
      // Generate top level config.
      $files = array('solr.xml' => theme('searchlight_solr_cores', $cores));
      $this->solrWriteFiles($file_path, $files);
    }
    else {
      return drush_log("/{$filepath} could not be written to.", 'error');
    }
  }

  /**
   * Generate an array representing the conf and index settings for a datasource.
   */
  function solrDatasourceConf($datasource) {

    // Configuration options.
    $conf = array('schema' => array(), 'conf' => array());
    $conf['conf']['id'] = $datasource['id'];

    // Determine the primary id field of the view & determine if it is present.
    $base_table = views_fetch_data($datasource['base_table']);
    $base_schema = drupal_get_schema($datasource['base_table']);
    // $base_field = $base_schema['field'][$base_table['table']['base']['field']];
    foreach ($datasource['fields'] as $field) {
      if ($field['table'] == $datasource['base_table'] && $field['name'] == $base_table['table']['base']['field']) {
        $conf['conf']['unique_key'] = 'true';
        break;
      }
    }

    // Add the primary field if it isn't present
    if (!isset($conf['conf']['unique_key'])) {
      $datasource['fields'][] = array(
        'name' => $datasource['base_table'] .'_'. $base_table['table']['base']['field'],
        'datatype' => 'int',
        // 'required' => true,
      );
      $conf['conf']['unique_key'] = $datasource['base_table'] .'_'. $base_table['table']['base']['field'];
    }

    // Attributes: name, type, indexed, stored, required, multiValued, omitNorms
    foreach ($datasource['fields'] as $field) {
      $f = $field['name'];
      $conf['schema'][$f] = array(
        'name' => $f,
      );
      switch ($field['datatype']) {
        case 'int':
          $conf['schema'][$f]['type'] = 'integer';
          break;
        case 'text':
          $conf['schema'][$f]['type'] = 'string';
          break;
        case 'timestamp':
          $conf['schema'][$f]['type'] = 'date';
      }

      if ($field['usage'] == 'multivalue') {
        $conf['schema'][$f]['multiValued'] = TRUE;
      }
    }
    return $conf;
  }

  /**
   * Write config to the filesystem
   */
  function solrWriteFiles($path, $files) {
    foreach($files as $name => $contents) {
      if ($contents) {
        $existing = '';
        if (file_exists("{$path}/{$name}")) {
          $existing = file_get_contents("{$path}/{$name}");
        }
        if ($contents !== $existing) {
         file_put_contents("{$path}/{$name}", $contents);
         drush_log("{$path}/{$name} was written successfully.", 'success');
        }
        else {
          drush_log("{$path}/{$name} is unchanged.", 'success');
        }
      }
      else {
        drush_log("{$path}/{$name} was not generated.", 'warning');
      }
    }
  }

  /**
   * Include the solr PHP API library.
   */
  function includeSolr() {
    module_load_include('php', 'searchlight', 'libraries/SolrPhpClient/Apache/Solr/Service.php');
  }

  /**
   * Get the ordinal integer for a given string, or vice versa.
   */
  function getOrdinal($datasource, $attribute_name, $value, $reset = FALSE) {
    $datasource_id = $datasource->id;

    if (!isset($this->ordinals[$datasource_id]) || $reset) {
      $cid = "searchlight_solr_ordinals_{$datasource_id}";
      if (!$reset && $cache = cache_get($cid)) {
        $this->ordinals[$datasource_id] = $cache->data;
      }
      if ($reset || !isset($this->ordinals[$datasource_id])) {
        $mapping = $this->getOrdinals($datasource);
        if (!empty($mapping)) {
          cache_set($cid, $mapping);
        }
        $this->ordinals[$datasource_id] = $mapping;
      }
    }
    if (is_numeric($value)) {
      return array_search($value, $this->ordinals[$datasource_id][$attribute_name]);
    }
    else {
      return isset($this->ordinals[$datasource_id][$attribute_name][$value]) ? $this->ordinals[$datasource_id][$attribute_name][$value] : FALSE;
    }
  }

  /**
   * Get an ordinal mapping for this datasource.
   * As this is a very expensive operation, its results are cached and
   * used in the ->getOrdinal() method. You should only call this method
   * if you know what you are doing.
   */
  function getOrdinals($datasource) {
    // Retrieve all ordinal attributes.
    $attributes = array();
    if (!empty($datasource['fields'])) {
      foreach ($datasource['fields'] as $name => $field) {
        if ($field['datatype'] === 'text' && $field['usage'] === 'attribute') {
          $attributes[$name] = $field;
        }
      }
    }

    // If there are attributes with ordinals, generate a mapping.
    // We only handle string ordinals for sets of less than 1000 (for now).
    $ordinals = array();
    if (!empty($attributes)) {
      $this->includeSolr();
      $solr = new solrClient();
      $solr->SetLimits(0, 1000);

      foreach ($attributes as $name => $field) {
        // Dispatch a solr query to retrieve the ordinal.
        $solr->SetGroupBy($name, SPH_GROUPBY_ATTR, '@count desc');
        $solr->SetGroupDistinct($name);
        $result = $solr->Query(NULL, $datasource['id']);

        if (count($result['matches']) < 1000) {
          // Dispatch a Views query to retrieve the corresponding string.
          $ids = implode(',', array_keys($result['matches']));
          $view = searchlight_build_view($datasource['view']);
          $view->set_items_per_page(0);
          $view->query->where = array();
          $view->query->add_where(0, "{$view->base_table}.{$view->base_field} IN ({$ids})");
          $view->build_info['query'] = $view->query->query();
          $view->build_info['query_args'] = $view->query->get_where_args();
          $view->execute();
          foreach ($view->result as $row) {
            $id = $row->{$view->base_field};
            if (isset($result['matches'][$id], $row->{$name})) {
              $ordinals[$name][$row->{$name}] = $result['matches'][$id]['attrs'][$name];
            }
          }
        }
      }
    }
    return $ordinals;
  }

  /**
   * Convert a solr datestring into a timestamp. Requires a datestring
   * (e.g. 201010) and a target granularity: day, month, year. We reverse-apply
   * Drupal's timezone offset so that this date can be used with format_date(),
   * etc. cleanly.
   */
  function getTimestamp($datestring, $granularity) {
    global $user;
    if (variable_get('configurable_timezones', 1) && $user->uid && strlen($user->timezone)) {
      $timezone = $user->timezone;
    }
    else {
      $timezone = variable_get('date_default_timezone', 0);
    }
    switch ($granularity) {
      case 'day':
        return ($timezone * -1) + mktime(0, 0, 0, substr($datestring, 4, 2), substr($datestring, 6, 2), substr($datestring, 0, 4));
      case 'month':
        return ($timezone * -1) + mktime(0, 0, 0, substr($datestring, 4, 2), 1, substr($datestring, 0, 4));
      case 'year':
        return ($timezone * -1) + mktime(0, 0, 0, 1, 1, $datestring);
    }
  }
}
