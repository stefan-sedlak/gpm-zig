int old_main()
{
   int ctlfd, newfd;
   struct sockaddr_un ctladdr;
   int i, len, kd_mode, fd;
   struct timeval timeout;
   int maxfd=-1;
   int pending;
   Gpm_Event event;

   for (i = 1; i <= 1+opt_double; i++) {
      which_mouse=mouse_table+i; /* used to access options */

      if (!opt_dev) gpm_report(GPM_PR_OOPS,GPM_MESS_NEED_MDEV);

      if(!strcmp(opt_dev,"-")) fd=0; /* use stdin */
      else if( (fd=open(opt_dev,O_RDWR | O_NDELAY)) < 0)
         gpm_report(GPM_PR_OOPS,GPM_MESS_OPEN,opt_dev); 
             
      /* and then reset the flag */
      fcntl(fd,F_SETFL,fcntl(fd,F_GETFL) & ~O_NDELAY);

      /* create argc and argv for this device */
      mouse_argv[i] = build_argv(opt_type, opt_options, &mouse_argc[i], ',');

      /* init the device, and use the return value as new mouse type */
      if (m_type->init)
         m_type=(m_type->init)(fd, m_type->flags, m_type, mouse_argc[i],
         mouse_argv[i]);
      if (!m_type) gpm_report(GPM_PR_OOPS,GPM_MESS_MOUSE_INIT);

      which_mouse->fd=fd;
      maxfd=max(fd, maxfd);
   }

/*....................................... catch interesting signals */

   signal(SIGTERM, gpm_killed);
   signal(SIGINT,  gpm_killed);
   signal(SIGUSR1, gpm_killed); /* usr1 is used by a new gpm killing the old */
   signal(SIGWINCH,gpm_killed); /* winch can be sent if console is resized */

/*....................................... create your nodes */

   /* control node */

   if((ctlfd=socket(AF_UNIX,SOCK_STREAM,0))==-1) gpm_report(GPM_PR_OOPS,GPM_MESS_SOCKET_PROB);
   bzero((char *)&ctladdr,sizeof(ctladdr));
   ctladdr.sun_family=AF_UNIX;
   strcpy(ctladdr.sun_path,GPM_NODE_CTL);
   unlink(GPM_NODE_CTL);

   len=sizeof(ctladdr.sun_family)+strlen(GPM_NODE_CTL);
   if(bind(ctlfd,(struct sockaddr *)(&ctladdr),len) == -1)
      gpm_report(GPM_PR_OOPS,GPM_MESS_BIND_PROB,ctladdr.sun_path);
   maxfd=max(maxfd,ctlfd);

   /* needs to be 0777, so all users can _try_ to access gpm */
   chmod(GPM_NODE_CTL,0777);

   get_console_size(&event); /* get screen dimensions */

/*....................................... wait for mouse and connections */

   listen(ctlfd, 5);          /* Queue up calls */

#define NULL_SET ((fd_set *)NULL)
#define resetTimeout() (timeout.tv_sec=SELECT_TIME,timeout.tv_usec=0)

   FD_ZERO(&connSet);
   FD_SET(ctlfd,&connSet);

   if (opt_double) FD_SET(mouse_table[2].fd,&connSet);

   readySet=connSet;
   FD_SET(mouse_table[1].fd,&readySet);

   signal(SIGPIPE,SIG_IGN);  /* WARN */

/*--------------------------------------- main loop begins here */

   while(1) {
      selSet=readySet;
      resetTimeout();
      if (opt_test) timeout.tv_sec=0;

      if (eventFlag) { /* an event left over by clustering */
         pending=1;
         FD_ZERO(&selSet);
         FD_SET(mouse_table[eventFlag].fd,&selSet);
      }
      else
         while((pending=select(maxfd+1,&selSet,NULL_SET,NULL_SET,&timeout))==0){
            selSet=readySet;
            resetTimeout();
         } /* go on */

      if(opt_resize) { /* did the console resize? */
         get_console_size(&event);
         opt_resize--;
         signal(SIGWINCH,gpm_killed); /* reinstall handler */

         /* and notify clients */
         for(i=0; i<MAX_VC+1; i++) {
            Gpm_Cinfo *ci;
            for (ci = cinfo[i]; ci; ci = ci->next) kill(ci->data.pid,SIGWINCH);
         }
      }

      if (pending < 0) {
         if (errno==EBADF) gpm_report(GPM_PR_OOPS,GPM_MESS_SELECT_PROB);
         gpm_report(GPM_PR_ERR,GPM_MESS_SELECT_STRING,strerror(errno));
         selSet=readySet;
         resetTimeout();
         continue;
      }

      gpm_report(GPM_PR_DEBUG,GPM_MESS_SELECT_TIMES,pending);

/*....................................... manage graphic mode */

     /* 
      * Be sure to be in text mode. This used to be before select,
      * but actually it only matters if you have events.
      */
      {
      int fd = open_console(O_RDONLY);
      if (ioctl(fd, KDGETMODE, &kd_mode) < 0)
         gpm_report(GPM_PR_OOPS,GPM_MESS_IOCTL_KDGETMODE);
      close(fd);
      if(kd_mode != KD_TEXT && !option.repeater) {
         wait_text(&mouse_table[1].fd);
         maxfd=max(maxfd,mouse_table[1].fd);
         readySet=connSet;
         FD_SET(mouse_table[1].fd,&readySet);
         continue; /* reselect */
      }
      }

/*....................................... got mouse, process event */
/*
 * Well, actually, run a loop to maintain inlining of functions without
 * lenghtening the file. This is not too clean a code, but it works....
 */

      for (i=1; i <= 1+opt_double; i++) {
         which_mouse=mouse_table+i; /* used to access options */
         if (FD_ISSET(which_mouse->fd,&selSet)) {
            FD_CLR(which_mouse->fd,&selSet); pending--;
            if (processMouse(which_mouse->fd, &event, m_type, kd_mode))
               /* pass it to the client, if any
                * or to the default handler, if any
                * or to the selection handler
                */ /* FIXME -- check event.vc */
               /* can't we please rewrite the following a bit nicer?*/
               (cinfo[event.vc] && do_client(cinfo[event.vc], &event))
               || (cinfo[0]        && do_client(cinfo[0],        &event))
               ||  do_selection(&event);
            }
      }

      /*..................... got connection, process it */

      if (pending && FD_ISSET(ctlfd,&selSet)) {
         FD_CLR(ctlfd,&selSet); pending--;
         newfd=processConn(ctlfd);
         if (newfd>=0) {
            FD_SET(newfd,&connSet);
            FD_SET(newfd,&readySet);
            maxfd=max(maxfd,newfd);
         }
      }

      /*........................ got request */

     /* itz 10-22-96 check _all_ clients, not just those on top! */
      for (i=0; pending && (i<=MAX_VC); i++) {
         Gpm_Cinfo* ci;
         for (ci = cinfo[i]; pending && ci; ci = ci->next) {
            if (FD_ISSET(ci->fd,&selSet)) {
               FD_CLR(ci->fd,&selSet); pending--;
           /* itz Sat Sep 12 21:10:22 PDT 1998 */
           /* this code is clearly incorrect; the next highest
              descriptor after the one we're closing is not necessarily
              being used.  Fortunately, it doesn't hurt simply to leave this
              out. */

#ifdef NOTDEF
               if ((processRequest(ci,i)==-1) && maxfd==ci->fd) maxfd--;
#else
               (void)processRequest(ci,i);
#endif
            }
         }
      }

      /*.................. look for a spare fd */

      /* itz 10-22-96 this shouldn't happen now! */
      for (i=0; pending && i<=maxfd; i++) {
         if (FD_ISSET(i,&selSet)) {
            FD_CLR(i,&selSet);
            pending--;
            gpm_report(GPM_PR_WARN,GPM_MESS_STRANGE_DATA,i);
         }
      }
        
      /*................... all done. */
     
     if(pending) gpm_report(GPM_PR_OOPS,GPM_MESS_SELECT_PROB);
   } /* while(1) */
}
