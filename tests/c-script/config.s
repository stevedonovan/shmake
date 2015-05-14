char **cfg = config_read("llib.need");
FOR_SMAP(k,v,cfg)
   printf("%s: %s\n",k,v);
